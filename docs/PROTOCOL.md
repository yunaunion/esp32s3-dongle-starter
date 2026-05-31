# Management Protocol

Web UIとESP32-S3はUSB CDC上でJSON Linesを送受信します。1行が1 JSON objectです。

## Request

```json
{"id":"1","command":"dongle.status","params":{}}
```

- `id`: Web UIが付与する文字列
- `command`: コマンド名
- `params`: コマンド引数

## Response

```json
{"id":"1","ok":true,"data":{"firmware":"0.1.0"}}
```

```json
{"id":"2","ok":false,"error":{"code":"pair_failed","message":"BLE HID pairing could not be completed"}}
```

## Event

```json
{"event":"device.discovered","data":{"address":"AA:BB:CC:DD:EE:FF","addressType":"random","name":"BLE Mouse","rssi":-52,"kind":"mouse"}}
```

## Commands

### `hello`

Web UIとファームのプロトコル互換性を確認します。

```json
{"id":"1","command":"hello","params":{"protocol":1,"client":"github-pages"}}
```

### `dongle.status`

ファームバージョン、BLE状態、USB状態、保存件数を返します。

### `scan.start`

BLE scanを開始します。

```json
{"id":"2","command":"scan.start","params":{"durationMs":10000,"hidOnly":true}}
```

### `scan.stop`

BLE scanを停止します。

### `pair.start`

指定したBLE peripheralとのペアリングを開始します。

```json
{"id":"3","command":"pair.start","params":{"address":"AA:BB:CC:DD:EE:FF","addressType":"random"}}
```

### `bond.list`

保存済みデバイス一覧を返します。

### `bond.delete`

保存済みデバイスを削除します。

```json
{"id":"4","command":"bond.delete","params":{"id":"dev-001"}}
```

### `connect`

保存済みデバイスへ接続します。

### `disconnect`

保存済みデバイスを切断します。

### `policy.set`

自動接続や表示名などの管理メタデータを更新します。

```json
{"id":"5","command":"policy.set","params":{"id":"dev-001","autoConnect":true,"label":"Desk Mouse"}}
```
