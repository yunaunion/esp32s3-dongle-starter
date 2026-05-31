# ESP32-S3 BLE HID Dongle Starter

ESP32-S3を「PCへはUSB複合デバイス、周辺機器へはBLE Central」として動かし、BLE HIDマウス/キーボード/ゲームパッドのペアリング情報をドングル本体へ保持するためのスターターです。

## 重要な前提

- ESP32-S3はBluetooth LE対応ですが、Bluetooth Classic(BR/EDR)には対応しません。
- まず狙うべきMVPはBLE HID(HOGP)のキーボード、マウス、ゲームパッドです。
- 一般的なBluetoothイヤホンのA2DP/HFPはBluetooth Classic系なので、ESP32-S3単体では対象外です。
- Web管理UIはGitHub Pages上のHTTPSページからWeb Serial APIでESP32-S3のUSB CDCへ接続する方針です。

## ディレクトリ

- `web/`: GitHub Pages向けの静的Web管理UI
- `firmware/`: ESP-IDF向けファームウェア骨組み
- `tools/mock-dongle.cjs`: JSON Lines管理プロトコルの開発用モック
- `docs/ARCHITECTURE.md`: 実現方針、制約、MVP、拡張案
- `docs/PROTOCOL.md`: Web UIとファーム間のJSON Linesプロトコル
- `docs/NEXT_STEPS.md`: 実機検証と次の実装順

## 最短の進め方

1. `web/`をGitHub Pagesで公開する、またはローカルでHTTPサーバを起動して開く。
2. ESP32-S3へ`firmware/`をESP-IDFでビルド/書き込みする。
3. Chrome/EdgeでWeb UIを開き、USB SerialとしてESP32-S3へ接続する。接続後は自動でBLE HIDスキャンを開始します。
4. BLE HID Host実装を`firmware/main/ble_hid_bridge.c`へ接続し、受信HID reportを`usb_hid_bridge.c`へ渡す。

## 現在の実装メモ

- Web管理画面の操作表示は日本語中心です。
- ESP32S3-ZeroのオンボードRGB LED(GPIO21)で状態を表示します。
- LED色は白=起動中、青=待機中、シアン=スキャン中、紫=ペアリング中、緑=接続あり、赤=エラーです。

## プロトコルだけ先に試す

Node.jsがある環境では、次のようにJSON Linesプロトコルのモックを動かせます。

```bash
node tools/mock-dongle.cjs
```

入力例:

```json
{"id":"1","command":"hello","params":{"protocol":1}}
{"id":"2","command":"scan.start","params":{"durationMs":10000,"hidOnly":true}}
```

## 公式情報メモ

- EspressifのESP-IDF Bluetooth Capability表では、ESP32-S3はBluetooth Classicが`N`、Bluetooth LEが`Y`です。
- EspressifのESP-IDF USB Device Stackでは、ESP32-S3でHID/CDCなどのUSBデバイス機能と複合デバイスが利用できます。
- MDNのWeb Serial APIはHTTPSなどのsecure contextが必要で、対応ブラウザが限定されます。GitHub PagesはHTTPSなので方針と相性が良いです。
