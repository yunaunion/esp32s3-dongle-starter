# Web管理画面

GitHub Pagesでそのまま公開できる、Web Serial対応の静的管理画面です。

## ローカル確認

プロジェクト直下から次を実行します。

```bash
python -m http.server 8765 --directory web
```

その後、ブラウザで開きます。

```text
http://127.0.0.1:8765/
```

Web Serialは対応Chromiumブラウザのセキュアコンテキストで動きます。`localhost`/`127.0.0.1`とGitHub PagesのHTTPSはどちらも条件を満たします。

## GitHub Pages

`.github/workflows/pages.yml`で`web/`をPagesへ公開します。ビルド手順は不要です。

## 使い方

`デモ`を押すと、実機なしで保存済みデバイスとスキャン結果を表示できます。

ファームを書き込んだESP32-S3を接続したら、`接続`を押してESP32-S3のシリアルポートを選びます。接続に成功すると自動でスキャンを開始します。

- `更新`: ドングル状態と保存済み一覧を再取得
- `開始`: BLE HIDスキャンを開始
- `停止`: BLE HIDスキャンを停止
- `ペアリング`: 検出した機器を保存
- `削除`: 保存済み機器を削除
