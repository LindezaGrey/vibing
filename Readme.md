## Remote HID Bridge for AtomS3 Lite (BLE → USB Keyboard/Mouse)

This project turns an M5Stack AtomS3 Lite (or similar ESP32‑S3 board with native USB) into a wireless Human Interface Device bridge:

- From a phone or laptop, a Web Bluetooth page sends keystrokes and mouse movements over BLE.
- The AtomS3 Lite receives those packets and immediately emits real USB HID keyboard/mouse events to the computer it’s plugged into.

Live web UI: https://lindezagrey.github.io/vibing/apps/atom-keyboard/index.html


## What the HID bridge is for

- Control a computer from across the room (presentations, HTPC, kiosk).
- Type short strings, press Enter/Backspace, and move/scroll the mouse.
- No drivers on the target computer; it sees a standard USB keyboard + mouse.

How it works in one line: Browser (Web Bluetooth) → BLE GATT → AtomS3 Lite → USB HID → Target computer.


## Quick start (no local build)

1) Flash the firmware to an AtomS3 Lite once (see Build/Flash below if needed).
2) Plug the AtomS3 Lite via USB‑C into the computer you want to control.
3) Open the web UI on a separate device with Bluetooth:
	 - https://lindezagrey.github.io/vibing/apps/atom-keyboard/index.html
	 - Click Connect and pick "Atom HID Bridge".
4) Type in the Keyboard box or use the Direct input field; use the Mouse pad and wheel buttons.

LED states on the AtomS3 Lite:
- Yellow: advertising / not connected.
- Blue: connected over BLE and ready.
- Red: Mouse Wiggler active.


## Requirements

Hardware
- M5Stack AtomS3 Lite (ESP32‑S3) or any ESP32‑S3 board that supports native USB HID.
- USB‑C cable to the target computer.
- A second device with Bluetooth LE (phone or laptop) to run the web UI.

Software (web UI side)
- A browser with Web Bluetooth support: Chrome/Chromium/Edge on desktop, Chrome on Android.
- Notes: macOS and Windows work well. Linux usually works if Bluetooth is configured (BlueZ). iOS Safari has limited/unstable support for Web Bluetooth.

Software (firmware build side)
- Arduino IDE 2.x (or PlatformIO) with Arduino-ESP32 for ESP32‑S3 (2.0.11+ or 3.x).
- Libraries: M5Unified, NimBLE-Arduino, Adafruit NeoPixel. The ESP32 core provides USB HID (TinyUSB) classes.


## Build and flash (Arduino IDE)

1) Install "ESP32 by Espressif Systems" board package (S3 supported).
2) Install libraries: M5Unified, NimBLE-Arduino, Adafruit NeoPixel.
3) Open `apps/atom-keyboard/atom-ble-bridge/atom-ble-bridge.ino`.
4) Board: "M5Stack AtomS3 Lite" (or your ESP32‑S3 equivalent). Keep default partition scheme.
5) Compile and Upload via USB.
6) After flashing, plug the board into the target computer you wish to control.

Notes
- The firmware enumerates as a composite USB HID Keyboard + HID Mouse + CDC serial. No special drivers are needed on common OSes.
- BLE has no pairing/auth here; keep the device in your proximity and unplug when not in use if security matters.


## Hosting the web UI locally (optional)

This repo includes a basic Nginx config to serve the static `apps` folder:

- Start the bundled task "Start nginx" to serve at http://localhost:8080/
- The Atom keyboard page will be at: http://localhost:8080/atom-keyboard/


## Mouse Wiggler (keep-awake)

Purpose
- Prevent the target computer’s screen from locking by periodically nudging the mouse position a tiny amount.

Behavior
- The device performs a minimal left-right 10 px “jiggle” roughly every 30 seconds to avoid visible drift.
- While active, the AtomS3 Lite LED glows red.

How to toggle
- Hardware: press the AtomS3 Lite front button to toggle ON/OFF. The LED turns red when ON.
- Web UI: open Settings and check “Enable mouse wiggler”. The header shows a small badge “wiggler: on/off”.

Notes
- Some corporate environments enforce lock policies that ignore input from non-user activity; effectiveness can vary.
- The interval is fixed in firmware by default and optimized to be subtle; you can adjust it in code if needed.


## Protocol details (for integrators)

BLE Service UUID: `5a1a0001-8f19-4a86-9a9e-7b4f7f9b0001`
- Keyboard characteristic (Write Without Response): `5a1a0002-8f19-4a86-9a9e-7b4f7f9b0001`
	- Send UTF‑8 bytes. Special handling: `\n`/`\r` → Enter, `\b` → Backspace.
- Mouse characteristic (Write Without Response): `5a1a0003-8f19-4a86-9a9e-7b4f7f9b0001`
	- 4‑byte packet: `[buttons, dx, dy, wheel]`, where `buttons` bitmask is L=1, R=2, M=4; `dx/dy/wheel` are int8.
- Wiggler characteristic (Read/Write/Notify): `5a1a0004-8f19-4a86-9a9e-7b4f7f9b0001`
	- Read returns a single byte: `0` (off) or `1` (on).
	- Write with `0`/`1` (binary or ASCII) to disable/enable.
	- Notifies on state changes (button press or remote write).


## Troubleshooting

- Can’t see the device when connecting: ensure Bluetooth is on, the LED is yellow (advertising), and you’re within range.
- Web Bluetooth blocked: use Chrome/Edge, and serve the page over HTTPS or from localhost.
- USB not typing on target: ensure the AtomS3 Lite is plugged into the target computer, not your control device.
- Linux: verify BlueZ is running; try another BLE adapter if discovery fails.


## Safety and scope

This tool is intended for personal/benign automation and demos. It provides no BLE authentication. Don’t use it where untrusted people could connect or where keystroke injection poses risk.