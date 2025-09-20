/*
  AtomS3-Lite: BLE GATT (Web Bluetooth) -> USB HID (Keyboard + Mouse) + Serial debug
  - Keyboard: write UTF-8 to kbd characteristic (prints + '\n' optional on frontend)
  - Mouse: 4-byte packet: [buttons, dx, dy, wheel] (int8 dx/dy/wheel)
    buttons bitmask: bit0=Left, bit1=Right, bit2=Middle
*/

#include <M5Unified.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
// TinyUSB core headers for low-level control request inspection
extern "C" {
#include "tusb.h"
}

// RGB LED (AtomS3 Lite has one WS2812/SK6812 LED). Using Adafruit_NeoPixel for reliability across cores.
// AtomS3 Lite LED data pin is GPIO35; adjust if your board revision differs.
#include <Adafruit_NeoPixel.h>
#ifndef ATOMS3_LED_PIN
#define ATOMS3_LED_PIN 35
#endif
#ifndef ATOMS3_LED_COUNT
#define ATOMS3_LED_COUNT 1
#endif
static Adafruit_NeoPixel g_led(ATOMS3_LED_COUNT, ATOMS3_LED_PIN, NEO_GRB + NEO_KHZ800);

static inline void ledShowRGB(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 32) {
  g_led.setBrightness(brightness);
  g_led.setPixelColor(0, g_led.Color(r, g, b));
  g_led.show();
}

static inline void ledBlueConnected() { ledShowRGB(0, 64, 255); }
static inline void ledYellowIdle()    { ledShowRGB(255, 180, 0); }
static inline void ledRedWiggler()    { ledShowRGB(255, 0, 32); }

// ---- BLE (NimBLE-Arduino recommended) ----
#include <NimBLEDevice.h>

static constexpr char SERVICE_UUID[]   = "5a1a0001-8f19-4a86-9a9e-7b4f7f9b0001";
static constexpr char KBD_CHAR_UUID[]  = "5a1a0002-8f19-4a86-9a9e-7b4f7f9b0001"; // Write (no resp)
static constexpr char MOUSE_CHAR_UUID[] = "5a1a0003-8f19-4a86-9a9e-7b4f7f9b0001"; // Write (no resp)
static constexpr char WIGGLE_CHAR_UUID[] = "5a1a0004-8f19-4a86-9a9e-7b4f7f9b0001"; // Read/Write/Notify (0/1)
static constexpr char DEVICE_NAME[]    = "Atom HID Bridge"; // Friendly name shown in scanners/browsers
// New: Host OS characteristic (read/write/notify): 2 bytes payload [osCode, sourceBits]
// osCode: 0=Unknown,1=Windows,2=macOS,3=Linux,4=Android,5=iOS,6=ChromeOS
// sourceBits: bit0=BLE self-report, bit1=USB heuristic (future)
static constexpr char HOST_OS_CHAR_UUID[] = "5a1a0005-8f19-4a86-9a9e-7b4f7f9b0001";

USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;

NimBLEServer* bleServer {nullptr};
NimBLECharacteristic* kbdChar {nullptr};
NimBLECharacteristic* mouseChar {nullptr};
NimBLECharacteristic* wiggleChar {nullptr};
NimBLECharacteristic* hostOsChar {nullptr};
volatile bool g_bleConnected = false;

// Mouse wiggler state
static volatile bool g_wigglerActive = false;
static uint32_t g_wiggleIntervalMs = 30000; // 30s default
static uint32_t g_lastWiggleMs = 0;
static bool g_wiggleDir = false; // toggle direction to avoid drift

// Host OS tracking
enum HostOS : uint8_t { OS_UNKNOWN=0, OS_WINDOWS=1, OS_MAC=2, OS_LINUX=3, OS_ANDROID=4, OS_IOS=5, OS_CHROMEOS=6 };
static volatile uint8_t g_hostOs = OS_UNKNOWN;      // current guess/selection
static volatile uint8_t g_hostOsSources = 0;        // bit0=BLE, bit1=USBheuristic
static volatile bool g_usbMounted = false;
static uint32_t g_usbMountedAtMs = 0;
static volatile bool g_sawHidClassReq = false;      // any HID class/control traffic observed

static inline void publishHostOs() {
  if (!hostOsChar) return;
  uint8_t v[2] = { g_hostOs, g_hostOsSources };
  hostOsChar->setValue(v, sizeof(v));
  hostOsChar->notify();
}

static inline void setHostOs(uint8_t os, uint8_t sourceBit) {
  bool changed = (g_hostOs != os) || ((g_hostOsSources & sourceBit) == 0);
  g_hostOs = os;
  g_hostOsSources |= sourceBit;
  if (changed) {
    Serial.printf("[HOST-OS] os=%u sources=0x%02x\n", g_hostOs, g_hostOsSources);
    publishHostOs();
  }
}

static inline void updateLedState() {
  if (g_wigglerActive) { ledRedWiggler(); return; }
  if (g_bleConnected) { ledBlueConnected(); return; }
  ledYellowIdle();
}

static void setWiggler(bool on) {
  g_wigglerActive = on;
  // Reflect in characteristic and notify
  if (wiggleChar) {
    uint8_t v = g_wigglerActive ? 1 : 0;
    wiggleChar->setValue(&v, 1);
    wiggleChar->notify();
  }
  updateLedState();
}

class KbdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    std::string s = c->getValue();
    if (s.empty()) return;
    Serial.printf("[KBD] len=%u\n", (unsigned)s.size());
    // Send: printable bytes via print(); map control chars to HID keys
    // Control handled: \b (0x08) -> Backspace, \n or \r -> Enter
    std::string acc;
    auto flush = [&]() {
      if (!acc.empty()) {
        Keyboard.print(acc.c_str());
        acc.clear();
      }
    };
    for (size_t i = 0; i < s.size(); ++i) {
      const uint8_t b = static_cast<uint8_t>(s[i]);
      if (b == 0x08) { // backspace
        flush();
        Keyboard.write(KEY_BACKSPACE);
      } else if (b == '\n' || b == '\r') {
        flush();
        Keyboard.write(KEY_RETURN);
      } else {
        acc.push_back(static_cast<char>(b));
      }
    }
    flush();
  }
};

class MouseCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    std::string v = c->getValue();
    if (v.size() < 4) return;
    const uint8_t buttons = (uint8_t)v[0];
    const int8_t  dx = (int8_t)v[1];
    const int8_t  dy = (int8_t)v[2];
    const int8_t  wheel = (int8_t)v[3];

    Serial.printf("[MOUSE] btn=%u dx=%d dy=%d wh=%d\n", buttons, dx, dy, wheel);

    // Buttons: set exact state
    if (buttons & 0x01) Mouse.press(MOUSE_LEFT); else Mouse.release(MOUSE_LEFT);
    if (buttons & 0x02) Mouse.press(MOUSE_RIGHT); else Mouse.release(MOUSE_RIGHT);
    if (buttons & 0x04) Mouse.press(MOUSE_MIDDLE); else Mouse.release(MOUSE_MIDDLE);

    // Relative move & wheel
    if (dx || dy) Mouse.move(dx, dy, 0);
    if (wheel)    Mouse.move(0, 0, wheel);
  }
};

class WiggleCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    std::string v = c->getValue();
    bool on = false;
    if (!v.empty()) {
      // Accept single byte or ASCII '0'/'1'
      uint8_t b = (uint8_t)v[0];
      on = (b != 0 && b != '0');
    }
    setWiggler(on);
    Serial.printf("[WIGGLER] %s via BLE\n", on ? "ON" : "OFF");
  }
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    uint8_t v = g_wigglerActive ? 1 : 0;
    c->setValue(&v, 1);
  }
};

class HostOsCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    // Accept first byte as OS code; optional second byte for source mask from client (ignored for safety)
    std::string v = c->getValue();
    if (v.empty()) return;
    uint8_t code = (uint8_t)v[0];
    if (code > OS_CHROMEOS) code = OS_UNKNOWN;
    setHostOs(code, /*sourceBit=*/0x01); // mark BLE self-report
  }
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    uint8_t v[2] = { g_hostOs, g_hostOsSources };
    c->setValue(v, sizeof(v));
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  // NimBLE onConnect callback (current signature)
  void onConnect(NimBLEServer* s, NimBLEConnInfo&) override {
    g_bleConnected = true;
  updateLedState();
    Serial.println("[BLE] Connected");
  }

  // NimBLE onDisconnect callback - current signature includes reason
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo&, int reason) override {
    g_bleConnected = false;
  updateLedState();
    Serial.printf("[BLE] Disconnected (reason=%d) — advertising\n", reason);
    NimBLEDevice::startAdvertising();
  }

  // Backward-compat overload for older NimBLE versions without reason param
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo&) {
    g_bleConnected = false;
  updateLedState();
    Serial.println("[BLE] Disconnected — advertising");
    NimBLEDevice::startAdvertising();
  }
};

void setup() {
  auto cfg = M5.config(); cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  delay(800);
  Serial.println("\n[BOOT] Remote HID bridge");

  // Init onboard RGB LED and set initial state (not connected)
  g_led.begin();
  ledYellowIdle();

  // USB composite: CDC + HID
  USB.begin();
  Keyboard.begin();
  Mouse.begin();
  Serial.println("[USB] HID ready");

  // BLE init
  NimBLEDevice::init(DEVICE_NAME);
  // Use the highest available TX power enum depending on core/library version
#if defined(ESP_PWR_LVL_P9)
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
#elif defined(ESP_PWR_LVL_P7)
  NimBLEDevice::setPower(ESP_PWR_LVL_P7);
#else
  // Fallback to library default if neither enum is defined
  // NimBLEDevice::setPower(); // uncomment if your library provides a default overload
#endif
  NimBLEDevice::setSecurityAuth(false, false, false);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  // Ensure we resume advertising even if callback is missed
  bleServer->advertiseOnDisconnect(true);

  NimBLEService* svc = bleServer->createService(SERVICE_UUID);

  kbdChar = svc->createCharacteristic(
    KBD_CHAR_UUID, NIMBLE_PROPERTY::WRITE_NR // write without response
  );
  kbdChar->setCallbacks(new KbdCallbacks());

  mouseChar = svc->createCharacteristic(
    MOUSE_CHAR_UUID, NIMBLE_PROPERTY::WRITE_NR
  );
  mouseChar->setCallbacks(new MouseCallbacks());

  wiggleChar = svc->createCharacteristic(
    WIGGLE_CHAR_UUID,
    (NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY)
  );
  wiggleChar->setCallbacks(new WiggleCallbacks());
  {
    uint8_t init = 0; wiggleChar->setValue(&init, 1);
  }

  // Host OS characteristic (optional but useful for behavior tweaks per OS)
  hostOsChar = svc->createCharacteristic(
    HOST_OS_CHAR_UUID,
    (NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY)
  );
  hostOsChar->setCallbacks(new HostOsCallbacks());
  {
    uint8_t init[2] = { OS_UNKNOWN, 0x00 };
    hostOsChar->setValue(init, sizeof(init));
  }

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  // Build a compact, standards-compliant ADV: Flags + Name + HID appearance
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setName(DEVICE_NAME);                 // Complete Local Name in ADV
  advData.setAppearance(0x03C0);                // Generic HID appearance
  adv->setAdvertisementData(advData);

  // Put the 128-bit service UUID in scan response to avoid exceeding 31 bytes
  NimBLEAdvertisementData scanData;
  scanData.addServiceUUID(SERVICE_UUID);
  adv->setScanResponseData(scanData);

  NimBLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising");
}

// --- TinyUSB: Inspect control requests to infer host OS ---
// Return false to let the default stack handle the request; we only observe.
extern "C" bool tud_control_request_cb(uint8_t rhport, tusb_control_request_t const* request) {
  (void)rhport;
  // Standard GET_DESCRIPTOR
  if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD && request->bRequest == TUSB_REQ_GET_DESCRIPTOR) {
    uint8_t desc_type  = tu_u16_high(request->wValue);
    uint8_t desc_index = tu_u16_low(request->wValue);
    if (desc_type == TUSB_DESC_STRING && desc_index == 0xEE) {
      // Microsoft OS String Descriptor index request → strongly indicates Windows
      setHostOs(OS_WINDOWS, 0x02);
    } else if (desc_type == TUSB_DESC_DEVICE_QUALIFIER) {
      // Device Qualifier request on a FS-only device is often seen on macOS during probing.
      // Treat as macOS if we haven't already identified Windows.
      if (g_hostOs != OS_WINDOWS) setHostOs(OS_MAC, 0x02);
    } else if (desc_type == TUSB_DESC_REPORT) {
      // HID Report descriptor requested — generic HID host traffic observed
      g_sawHidClassReq = true;
    }
  }
  // HID class-specific requests (to interface): mark seen to allow Linux fallback
  if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS) {
    // Common HID requests: GET_REPORT (0x01), GET_IDLE (0x02), SET_IDLE (0x0A), SET_PROTOCOL (0x0B)
    switch (request->bRequest) {
      case 0x01: case 0x02: case 0x0A: case 0x0B:
        g_sawHidClassReq = true;
        break;
      default: break;
    }
  }
  return false; // continue normal handling
}

extern "C" void tud_mount_cb(void) {
  g_usbMounted = true;
  g_usbMountedAtMs = millis();
  // Reset transient observation flags on each mount
  g_sawHidClassReq = false;
}

extern "C" void tud_umount_cb(void) {
  g_usbMounted = false;
  g_usbMountedAtMs = 0;
  g_sawHidClassReq = false;
}

void loop() {
  M5.update();
  // After mount, if we saw HID traffic but no strong OS signals within grace window, assume Linux/ChromeOS
  if (g_usbMounted && g_hostOs == OS_UNKNOWN && g_sawHidClassReq) {
    const uint32_t now = millis();
    // 1200 ms grace lets Windows/macOS send their signature probes first
    if (g_usbMountedAtMs && (now - g_usbMountedAtMs) > 1200) {
      // Distinguishing Linux vs ChromeOS reliably from USB probes alone is non-trivial; prefer Linux as default
      setHostOs(OS_LINUX, 0x02);
    }
  }
  // Button toggle for wiggler (single-press)
  if (M5.BtnA.wasPressed()) {
    setWiggler(!g_wigglerActive);
    Serial.printf("[WIGGLER] %s via Button\n", g_wigglerActive ? "ON" : "OFF");
  }

  // Periodic jiggle to keep host awake
  if (g_wigglerActive) {
    const uint32_t now = millis();
    if (now - g_lastWiggleMs >= g_wiggleIntervalMs) {
      g_lastWiggleMs = now;
      const int8_t step = g_wiggleDir ? 10 : -10;
      g_wiggleDir = !g_wiggleDir;
      // Two small opposite moves with a short gap to avoid visible drift
      Mouse.move(step, 0, 0);
      delay(120);
      Mouse.move(-step, 0, 0);
    }
  }

  // keep CDC alive / yield
  delay(10);
}
