# Web Manager

This is a static GitHub Pages-compatible Web Serial UI.

## Local Preview

From the project root:

```bash
python -m http.server 8765 --directory web
```

Then open:

```text
http://127.0.0.1:8765/
```

`localhost`/`127.0.0.1` and GitHub Pages HTTPS both satisfy the secure-context requirement for Web Serial in supported Chromium browsers.

## GitHub Pages

Publish the `web/` directory as the Pages root. No build step is required.

## Development Mode

Press the `デモ` button to populate mock paired devices and scan results without hardware.

When firmware is flashed, press `接続`, choose the ESP32-S3 serial port, then use:

- `更新` for `dongle.status` + `bond.list`
- `開始` for `scan.start`
- `ペアリング` for `pair.start`
- `削除` for `bond.delete`
