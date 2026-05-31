#!/usr/bin/env node

const readline = require("readline");

const mockDevices = [
  { name: "MX Keys Mini", address: "F1:4A:82:10:2D:91", addressType: "random", kind: "keyboard", rssi: -48 },
  { name: "BLE Trackball", address: "C8:5D:3A:EF:09:11", addressType: "random", kind: "mouse", rssi: -57 },
  { name: "8BitDo Lite BLE", address: "E0:12:44:98:77:21", addressType: "public", kind: "gamepad", rssi: -61 },
  { name: "Travel Mouse", address: "D3:7E:10:AA:40:0B", addressType: "random", kind: "mouse", rssi: -49 },
];

const bonds = [];

function send(message) {
  process.stdout.write(`${JSON.stringify(message)}\n`);
}

function ok(id, data = {}) {
  send({ id, ok: true, data });
}

function fail(id, code, message) {
  send({ id, ok: false, error: { code, message } });
}

function event(name, data = {}) {
  send({ event: name, data });
}

function makeId(address) {
  return `dev-${address.replaceAll(":", "")}`;
}

function status() {
  return {
    firmware: "0.1.0-mock",
    ble: "ready-mock",
    usb: "cdc+hid",
    pairedCount: bonds.length,
  };
}

function pair(address, addressType) {
  const found = mockDevices.find((device) => device.address === address) || {
    name: address,
    address,
    addressType: addressType || "random",
    kind: "hid",
  };
  const id = makeId(address);
  const existing = bonds.find((device) => device.id === id);

  if (existing) {
    existing.connected = true;
    return existing;
  }

  const bond = {
    id,
    name: found.name,
    label: "",
    address: found.address,
    addressType: found.addressType,
    kind: found.kind,
    autoConnect: true,
    connected: true,
  };
  bonds.unshift(bond);
  return bond;
}

function handle(request) {
  const id = request.id;
  const params = request.params || {};

  switch (request.command) {
    case "hello":
      ok(id, { protocol: 1, firmware: "0.1.0-mock" });
      break;
    case "dongle.status":
      ok(id, status());
      break;
    case "bond.list":
      ok(id, { devices: bonds });
      break;
    case "scan.start":
      ok(id, { scanning: true });
      for (const device of mockDevices) {
        event("device.discovered", device);
      }
      break;
    case "scan.stop":
      ok(id);
      break;
    case "pair.start":
      if (!params.address) {
        fail(id, "invalid_arg", "address is required");
        return;
      }
      pair(params.address, params.addressType);
      ok(id, { paired: true });
      event("bond.changed", { devices: bonds });
      break;
    case "bond.delete": {
      const index = bonds.findIndex((device) => device.id === params.id);
      if (index < 0) {
        fail(id, "not_found", "Device is not in the pairing store");
        return;
      }
      bonds.splice(index, 1);
      ok(id);
      event("bond.changed", { devices: bonds });
      break;
    }
    case "connect":
    case "disconnect": {
      const device = bonds.find((item) => item.id === params.id);
      if (!device) {
        fail(id, "not_found", "Device is not in the pairing store");
        return;
      }
      device.connected = request.command === "connect";
      ok(id);
      event("bond.changed", { devices: bonds });
      break;
    }
    case "policy.set": {
      const device = bonds.find((item) => item.id === params.id);
      if (!device) {
        fail(id, "not_found", "Device is not in the pairing store");
        return;
      }
      if (typeof params.autoConnect === "boolean") {
        device.autoConnect = params.autoConnect;
      }
      if (typeof params.label === "string") {
        device.label = params.label;
      }
      ok(id);
      event("bond.changed", { devices: bonds });
      break;
    }
    default:
      fail(id, "unknown_command", "Unknown command");
  }
}

readline.createInterface({ input: process.stdin }).on("line", (line) => {
  try {
    handle(JSON.parse(line));
  } catch (error) {
    event("protocol.error", { message: error.message });
  }
});

