# Firmware Notes

Target: ESP32-S3 with ESP-IDF.

## Build

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

This workspace machine did not have `idf.py` in `PATH`, so the project was not compiled here.
On Windows, keep the project close to the drive root while building. ESP-IDF can hit path-length limits when the project is nested deeply.

## Current Firmware Behavior

The current firmware is a development stub for the management protocol:

- Initializes NVS.
- Stores paired-device metadata in NVS namespace `dongle`, key `devices`.
- Exposes JSON Lines commands through the current console stream.
- Emits mock BLE HID scan results.
- Supports mock `pair.start`, `bond.delete`, `connect`, `disconnect`, and `policy.set`.
- Drives the ESP32S3-Zero onboard WS2812 RGB LED on GPIO21 as a status indicator.

## RGB LED状態表示

- 白: 起動中
- 青: 待機中
- シアン: スキャン中
- 紫: ペアリング中
- 緑: 保存済みデバイスが1台以上接続扱い
- 赤: コマンドまたはプロトコルエラー

The actual BLE HID Host and USB HID report bridge still need to be wired into:

- `main/ble_hid_bridge.c`
- `main/usb_hid_bridge.c`

## Real Implementation Checklist

1. Replace mock scan with NimBLE GAP discovery filtered by HID service UUID `0x1812`.
2. Connect to selected BLE peripheral and discover HID service characteristics.
3. Enable pairing/bonding and persist stack bond data in NVS.
4. Subscribe to HID input report notifications.
5. Parse report descriptors and map common keyboard/mouse/gamepad reports to USB HID reports.
6. Replace console-based CDC plumbing with TinyUSB CDC ACM in the final composite USB descriptor.
7. Send keyboard/mouse/gamepad reports from `usb_hid_bridge.c`.

## Hardware Notes

ESP32-S3 USB device pins are GPIO20(D+) and GPIO19(D-). If the dev board has separate USB and USB-UART/JTAG ports, use the native USB port for HID/CDC device testing.

ESP32-S3 has only one internal USB PHY shared with USB-Serial-JTAG. The final dongle hardware should plan flashing/debug access carefully while native USB device mode is active.
