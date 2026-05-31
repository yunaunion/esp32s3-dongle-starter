const PROTOCOL_VERSION = 1;

const COMMAND_LABELS = {
  hello: "接続確認",
  "dongle.status": "状態取得",
  "bond.list": "保存済み一覧取得",
  "scan.start": "スキャン開始",
  "scan.stop": "スキャン停止",
  "pair.start": "ペアリング",
  "bond.delete": "削除",
  connect: "接続",
  disconnect: "切断",
  "policy.set": "設定変更",
};

const KIND_LABELS = {
  hid: "HID機器",
  keyboard: "キーボード",
  mouse: "マウス",
  gamepad: "ゲームパッド",
};

const ADDRESS_TYPE_LABELS = {
  random: "ランダム",
  public: "公開",
};

const BLE_STATE_LABELS = {
  stub: "初期化中（開発版）",
  "ready-stub": "待機中（開発版）",
  "scanning-stub": "スキャン中（開発版）",
  "pairing-stub": "ペアリング中（開発版）",
  ready: "待機中",
  scanning: "スキャン中",
  pairing: "ペアリング中",
};

const USB_STATE_LABELS = {
  "cdc+hid": "管理通信 + HID",
};

const state = {
  connected: false,
  demo: false,
  status: {
    firmware: "-",
    ble: "-",
    usb: "-",
    pairedCount: 0,
  },
  paired: [],
  discovered: [],
  logs: [],
};

const els = {
  connectionBadge: document.querySelector("#connectionBadge"),
  connectButton: document.querySelector("#connectButton"),
  demoButton: document.querySelector("#demoButton"),
  refreshButton: document.querySelector("#refreshButton"),
  scanButton: document.querySelector("#scanButton"),
  stopScanButton: document.querySelector("#stopScanButton"),
  clearLogButton: document.querySelector("#clearLogButton"),
  firmwareValue: document.querySelector("#firmwareValue"),
  bleValue: document.querySelector("#bleValue"),
  usbValue: document.querySelector("#usbValue"),
  pairedCountValue: document.querySelector("#pairedCountValue"),
  pairedList: document.querySelector("#pairedList"),
  scanList: document.querySelector("#scanList"),
  logOutput: document.querySelector("#logOutput"),
  pairedTemplate: document.querySelector("#pairedDeviceTemplate"),
  scanTemplate: document.querySelector("#scanDeviceTemplate"),
};

class SerialTransport {
  constructor() {
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.decoder = new TextDecoder();
    this.encoder = new TextEncoder();
    this.buffer = "";
    this.nextId = 1;
    this.pending = new Map();
  }

  async connect() {
    if (!("serial" in navigator)) {
      throw new Error("このブラウザはWeb Serial APIに対応していません。ChromeまたはEdgeを使用してください。");
    }

    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: 115200 });
    this.writer = this.port.writable.getWriter();
    this.readLoop();
  }

  async disconnect() {
    for (const [, pending] of this.pending) {
      pending.reject(new Error("切断されました"));
    }
    this.pending.clear();

    if (this.reader) {
      await this.reader.cancel().catch(() => {});
      this.reader.releaseLock();
      this.reader = null;
    }
    if (this.writer) {
      this.writer.releaseLock();
      this.writer = null;
    }
    if (this.port) {
      await this.port.close().catch(() => {});
      this.port = null;
    }
  }

  async readLoop() {
    this.reader = this.port.readable.getReader();
    try {
      while (true) {
        const { value, done } = await this.reader.read();
        if (done) {
          break;
        }
        this.consume(this.decoder.decode(value, { stream: true }));
      }
    } catch (error) {
      logLine(`受信エラー: ${error.message}`);
    } finally {
      if (state.connected) {
        setConnected(false);
      }
    }
  }

  consume(chunk) {
    this.buffer += chunk;
    let newlineIndex = this.buffer.indexOf("\n");
    while (newlineIndex >= 0) {
      const line = this.buffer.slice(0, newlineIndex).trim();
      this.buffer = this.buffer.slice(newlineIndex + 1);
      if (line) {
        handleMessage(line);
      }
      newlineIndex = this.buffer.indexOf("\n");
    }
  }

  async request(command, params = {}) {
    const id = String(this.nextId++);
    const payload = { id, command, params };
    const line = `${JSON.stringify(payload)}\n`;
    logLine(`送信 ${line.trim()}`);
    await this.writer.write(this.encoder.encode(line));

    return new Promise((resolve, reject) => {
      const timer = window.setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${commandLabel(command)}がタイムアウトしました`));
      }, 8000);
      this.pending.set(id, { resolve, reject, timer });
    });
  }

  resolve(message) {
    const pending = this.pending.get(message.id);
    if (!pending) {
      return;
    }
    window.clearTimeout(pending.timer);
    this.pending.delete(message.id);
    if (message.ok) {
      pending.resolve(message.data ?? {});
      return;
    }
    pending.reject(new Error(message.error?.message || "コマンドが失敗しました"));
  }
}

const transport = new SerialTransport();

els.connectButton.addEventListener("click", async () => {
  if (state.connected) {
    await transport.disconnect();
    setConnected(false);
    return;
  }

  try {
    await transport.connect();
    setConnected(true);
    const hello = await request("hello", { protocol: PROTOCOL_VERSION, client: "github-pages" });
    logLine(`接続確認 ${JSON.stringify(hello)}`);
    await refreshAll();
    await startScan({ automatic: true });
  } catch (error) {
    setConnected(false, true);
    logLine(`接続失敗: ${error.message}`);
  }
});

els.demoButton.addEventListener("click", () => {
  state.demo = !state.demo;
  if (state.demo) {
    loadDemoState();
  } else {
    state.paired = [];
    state.discovered = [];
    state.status = { firmware: "-", ble: "-", usb: "-", pairedCount: 0 };
  }
  render();
});

els.refreshButton.addEventListener("click", refreshAll);
els.scanButton.addEventListener("click", () => startScan());
els.stopScanButton.addEventListener("click", () => stopScan());
els.clearLogButton.addEventListener("click", () => {
  state.logs = [];
  renderLog();
});

async function request(command, params = {}) {
  if (state.demo) {
    return demoRequest(command, params);
  }
  if (!state.connected) {
    throw new Error("ドングルに接続していません。");
  }
  return transport.request(command, params);
}

async function refreshAll() {
  try {
    const status = await request("dongle.status");
    applyStatus(status);
    const list = await request("bond.list");
    state.paired = list.devices ?? [];
    state.status.pairedCount = state.paired.length;
    render();
  } catch (error) {
    showError(error);
  }
}

async function startScan(options = {}) {
  try {
    state.discovered = [];
    renderScanList();
    await request("scan.start", { durationMs: 10000, hidOnly: true });
    logLine(options.automatic ? "接続後の自動スキャンを開始しました" : "スキャンを開始しました");
  } catch (error) {
    showError(error);
  }
}

async function stopScan() {
  try {
    await request("scan.stop");
    logLine("スキャンを停止しました");
  } catch (error) {
    showError(error);
  }
}

async function pairDevice(device) {
  try {
    await request("pair.start", { address: device.address, addressType: device.addressType });
    await refreshAll();
  } catch (error) {
    showError(error);
  }
}

async function deleteDevice(device) {
  try {
    await request("bond.delete", { id: device.id });
    state.paired = state.paired.filter((item) => item.id !== device.id);
    state.status.pairedCount = state.paired.length;
    render();
  } catch (error) {
    showError(error);
  }
}

async function setPolicy(device, patch) {
  try {
    await request("policy.set", { id: device.id, ...patch });
    state.paired = state.paired.map((item) => item.id === device.id ? { ...item, ...patch } : item);
    renderPairedList();
  } catch (error) {
    showError(error);
  }
}

function handleMessage(line) {
  logLine(`受信 ${line}`);
  let message;
  try {
    message = JSON.parse(line);
  } catch (error) {
    logLine(`JSON解析エラー: ${error.message}`);
    return;
  }

  if (message.id) {
    transport.resolve(message);
    return;
  }

  if (message.event === "device.discovered") {
    upsertDiscovered(message.data);
  }
  if (message.event === "bond.changed") {
    refreshAll();
  }
  if (message.event === "status.changed") {
    applyStatus(message.data);
    render();
  }
}

function applyStatus(status) {
  state.status = {
    firmware: status.firmware ?? "-",
    ble: status.ble ?? "-",
    usb: status.usb ?? "-",
    pairedCount: status.pairedCount ?? state.paired.length,
  };
}

function commandLabel(command) {
  return COMMAND_LABELS[command] ?? command;
}

function kindLabel(kind) {
  return KIND_LABELS[kind] ?? (kind || "HID機器");
}

function addressTypeLabel(addressType) {
  return ADDRESS_TYPE_LABELS[addressType] ?? (addressType || "-");
}

function bleStateLabel(value) {
  return BLE_STATE_LABELS[value] ?? (value || "-");
}

function usbStateLabel(value) {
  return USB_STATE_LABELS[value] ?? (value || "-");
}

function upsertDiscovered(device) {
  const key = `${device.addressType}:${device.address}`;
  const next = state.discovered.filter((item) => `${item.addressType}:${item.address}` !== key);
  next.unshift(device);
  state.discovered = next.slice(0, 30);
  renderScanList();
}

function setConnected(connected, error = false) {
  state.connected = connected;
  els.connectButton.textContent = connected ? "切断" : "接続";
  els.connectionBadge.textContent = connected ? "接続中" : error ? "エラー" : "未接続";
  els.connectionBadge.className = `badge ${connected ? "badge-online" : error ? "badge-error" : "badge-idle"}`;
  renderControls();
}

function render() {
  els.firmwareValue.textContent = state.status.firmware;
  els.bleValue.textContent = bleStateLabel(state.status.ble);
  els.usbValue.textContent = usbStateLabel(state.status.usb);
  els.pairedCountValue.textContent = String(state.status.pairedCount);
  renderControls();
  renderPairedList();
  renderScanList();
  renderLog();
}

function renderControls() {
  const enabled = state.connected || state.demo;
  els.refreshButton.disabled = !enabled;
  els.scanButton.disabled = !enabled;
  els.stopScanButton.disabled = !enabled;
  els.demoButton.textContent = state.demo ? "デモ終了" : "デモ";
}

function renderPairedList() {
  els.pairedList.innerHTML = "";
  if (!state.paired.length) {
    els.pairedList.append(emptyState("保存済みデバイスはありません"));
    return;
  }

  for (const device of state.paired) {
    const row = els.pairedTemplate.content.firstElementChild.cloneNode(true);
    row.querySelector(".device-name").textContent = device.label || device.name || device.address;
    row.querySelector(".device-meta").textContent = [
      kindLabel(device.kind),
      device.connected ? "接続中" : "待機中",
      device.address,
    ].filter(Boolean).join(" / ");

    const autoConnect = row.querySelector(".auto-connect");
    autoConnect.checked = Boolean(device.autoConnect);
    autoConnect.addEventListener("change", () => setPolicy(device, { autoConnect: autoConnect.checked }));

    row.querySelector(".connect-device").addEventListener("click", () => request("connect", { id: device.id }).then(refreshAll).catch(showError));
    row.querySelector(".disconnect-device").addEventListener("click", () => request("disconnect", { id: device.id }).then(refreshAll).catch(showError));
    row.querySelector(".delete-device").addEventListener("click", () => deleteDevice(device));
    els.pairedList.append(row);
  }
}

function renderScanList() {
  els.scanList.innerHTML = "";
  if (!state.discovered.length) {
    els.scanList.append(emptyState("検出されたBLE HID機器はありません"));
    return;
  }

  for (const device of state.discovered) {
    const row = els.scanTemplate.content.firstElementChild.cloneNode(true);
    row.querySelector(".device-name").textContent = device.name || device.address;
    row.querySelector(".device-meta").textContent = [
      kindLabel(device.kind),
      addressTypeLabel(device.addressType),
      device.address,
      Number.isFinite(device.rssi) ? `${device.rssi} dBm` : "",
    ].filter(Boolean).join(" / ");
    row.querySelector(".pair-device").addEventListener("click", () => pairDevice(device));
    els.scanList.append(row);
  }
}

function renderLog() {
  els.logOutput.textContent = state.logs.join("\n");
  els.logOutput.scrollTop = els.logOutput.scrollHeight;
}

function emptyState(text) {
  const element = document.createElement("div");
  element.className = "empty-state";
  element.textContent = text;
  return element;
}

function logLine(text) {
  const stamp = new Date().toLocaleTimeString("ja-JP", { hour12: false });
  state.logs.push(`[${stamp}] ${text}`);
  state.logs = state.logs.slice(-200);
  renderLog();
}

function showError(error) {
  logLine(`エラー: ${error.message}`);
}

function loadDemoState() {
  state.status = {
    firmware: "0.1.0-demo",
    ble: "ready",
    usb: "cdc+hid",
    pairedCount: 2,
  };
  state.paired = [
    {
      id: "dev-kbd-001",
      name: "MX Keys Mini",
      label: "デスク用キーボード",
      address: "F1:4A:82:10:2D:91",
      addressType: "random",
      kind: "keyboard",
      autoConnect: true,
      connected: true,
    },
    {
      id: "dev-mouse-001",
      name: "BLE Trackball",
      address: "C8:5D:3A:EF:09:11",
      addressType: "random",
      kind: "mouse",
      autoConnect: true,
      connected: false,
    },
  ];
  state.discovered = [
    {
      name: "8BitDo Lite BLE",
      address: "E0:12:44:98:77:21",
      addressType: "public",
      kind: "gamepad",
      rssi: -61,
    },
    {
      name: "Travel Mouse",
      address: "D3:7E:10:AA:40:0B",
      addressType: "random",
      kind: "mouse",
      rssi: -49,
    },
  ];
  logLine("デモデータを読み込みました");
}

async function demoRequest(command, params) {
  await new Promise((resolve) => window.setTimeout(resolve, 180));
  logLine(`デモ処理: ${commandLabel(command)} ${JSON.stringify(params)}`);

  if (command === "dongle.status") {
    return state.status;
  }
  if (command === "bond.list") {
    return { devices: state.paired };
  }
  if (command === "scan.start") {
    window.setTimeout(() => upsertDiscovered({
      name: "BLE Number Pad",
      address: "A4:C1:38:22:19:7F",
      addressType: "random",
      kind: "keyboard",
      rssi: -55,
    }), 300);
    return { scanning: true };
  }
  if (command === "pair.start") {
    const found = state.discovered.find((item) => item.address === params.address);
    if (found) {
      state.paired.unshift({
        id: `dev-${Date.now()}`,
        ...found,
        autoConnect: true,
        connected: true,
      });
      state.status.pairedCount = state.paired.length;
    }
    return { paired: true };
  }
  if (command === "bond.delete") {
    return { deleted: true };
  }
  if (command === "connect" || command === "disconnect") {
    state.paired = state.paired.map((item) => item.id === params.id ? { ...item, connected: command === "connect" } : item);
    return { ok: true };
  }
  if (command === "policy.set") {
    state.paired = state.paired.map((item) => item.id === params.id ? { ...item, ...params } : item);
    return { ok: true };
  }
  if (command === "scan.stop") {
    return { ok: true };
  }
  return {};
}

render();
