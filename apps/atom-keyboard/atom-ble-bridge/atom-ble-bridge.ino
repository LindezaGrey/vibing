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

// ---- BLE (NimBLE-Arduino recommended) ----
#include <NimBLEDevice.h>

static constexpr char SERVICE_UUID[]   = "5a1a0001-8f19-4a86-9a9e-7b4f7f9b0001";
static constexpr char KBD_CHAR_UUID[]  = "5a1a0002-8f19-4a86-9a9e-7b4f7f9b0001"; // Write (no resp)
static constexpr char MOUSE_CHAR_UUID[] = "5a1a0003-8f19-4a86-9a9e-7b4f7f9b0001"; // Write (no resp)
static constexpr char DEVICE_NAME[]    = "Atom HID Bridge"; // Friendly name shown in scanners/browsers

USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;

NimBLEServer* bleServer {nullptr};
NimBLECharacteristic* kbdChar {nullptr};
NimBLECharacteristic* mouseChar {nullptr};
volatile bool g_bleConnected = false;

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

class ServerCallbacks : public NimBLEServerCallbacks {
  // Newer NimBLE versions provide NimBLEConnInfo&
  void onConnect(NimBLEServer* s, NimBLEConnInfo&) {
    g_bleConnected = true;
    ledBlueConnected();
    Serial.println("[BLE] Connected");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo&) {
    g_bleConnected = false;
    ledYellowIdle();
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

  NimBLEService* svc = bleServer->createService(SERVICE_UUID);

  kbdChar = svc->createCharacteristic(
    KBD_CHAR_UUID, NIMBLE_PROPERTY::WRITE_NR // write without response
  );
  kbdChar->setCallbacks(new KbdCallbacks());

  mouseChar = svc->createCharacteristic(
    MOUSE_CHAR_UUID, NIMBLE_PROPERTY::WRITE_NR
  );
  mouseChar->setCallbacks(new MouseCallbacks());

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

void loop() {
  // nothing: everything is event-driven; keep CDC alive
  delay(10);
}
