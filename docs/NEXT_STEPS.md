# Next Steps

## What Is Ready

- Static Web UI for GitHub Pages.
- Web Serial JSON Lines transport.
- Demo mode for UI development.
- Node.js mock for the management protocol.
- ESP-IDF project skeleton.
- NVS-backed paired-device metadata store.
- Firmware development stub for scan/pair/delete/connect/policy commands.

## What Needs Real Hardware

1. Install ESP-IDF and build `firmware/`.
2. Flash an ESP32-S3 board through its native USB path.
3. Open `http://127.0.0.1:8765/` or the GitHub Pages URL in Chrome/Edge.
4. Connect Web Serial to the ESP32-S3.
5. Confirm mock scan and NVS pairing metadata survive reboot.
6. Replace the mock BLE scan/pair layer with NimBLE HID Host behavior.
7. Replace the USB stub with a TinyUSB CDC + HID composite descriptor.

## References

- ESP-IDF Bluetooth capability table: ESP32-S3 supports Bluetooth LE and does not support Bluetooth Classic.
  https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/bt-architecture/overview.html
- ESP32-S3 BLE overview: ESP32-S3 supports Bluetooth LE and Classic Bluetooth is not supported.
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/ble/overview.html
- ESP-IDF USB Device Stack: ESP32-S3 supports TinyUSB-based CDC/HID classes and composite devices.
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_device.html
- MDN Web Serial API secure context note.
  https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API

