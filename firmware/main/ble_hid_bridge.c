#include "ble_hid_bridge.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "manager_protocol.h"
#include "pairing_store.h"

typedef struct {
    const char *name;
    const char *address;
    const char *address_type;
    const char *kind;
    int rssi;
} mock_hid_device_t;

static const char *TAG = "ble_hid";
static const char *state = "stub";

static const mock_hid_device_t MOCK_DEVICES[] = {
    { "MX Keys Mini", "F1:4A:82:10:2D:91", "random", "keyboard", -48 },
    { "BLE Trackball", "C8:5D:3A:EF:09:11", "random", "mouse", -57 },
    { "8BitDo Lite BLE", "E0:12:44:98:77:21", "public", "gamepad", -61 },
    { "Travel Mouse", "D3:7E:10:AA:40:0B", "random", "mouse", -49 },
};

static const size_t MOCK_DEVICE_COUNT = sizeof(MOCK_DEVICES) / sizeof(MOCK_DEVICES[0]);

static const char *json_string(cJSON *object, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return "";
    }
    return item->valuestring;
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src != NULL ? src : "");
}

static void make_id(char *dst, size_t dst_size, const char *address)
{
    size_t out = 0;
    copy_text(dst, dst_size, "dev-");
    out = strlen(dst);

    for (const char *cursor = address; cursor != NULL && *cursor != '\0' && out + 1 < dst_size; ++cursor) {
        if (*cursor == ':') {
            continue;
        }
        dst[out++] = *cursor;
    }
    dst[out] = '\0';
}

static cJSON *mock_device_json(const mock_hid_device_t *device)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "name", device->name);
    cJSON_AddStringToObject(data, "address", device->address);
    cJSON_AddStringToObject(data, "addressType", device->address_type);
    cJSON_AddStringToObject(data, "kind", device->kind);
    cJSON_AddNumberToObject(data, "rssi", device->rssi);
    return data;
}

static const mock_hid_device_t *find_mock_device(const char *address)
{
    for (size_t index = 0; index < MOCK_DEVICE_COUNT; ++index) {
        if (strcmp(MOCK_DEVICES[index].address, address) == 0) {
            return &MOCK_DEVICES[index];
        }
    }
    return NULL;
}

esp_err_t ble_hid_bridge_init(void)
{
    state = "ready-stub";
    ESP_LOGI(TAG, "BLE HID bridge development stub initialized");
    return ESP_OK;
}

const char *ble_hid_bridge_state(void)
{
    return state;
}

esp_err_t ble_hid_bridge_scan_start(cJSON *params)
{
    (void)params;
    state = "scanning-stub";
    ESP_LOGI(TAG, "mock BLE HID scan started");

    for (size_t index = 0; index < MOCK_DEVICE_COUNT; ++index) {
        manager_protocol_emit_event("device.discovered", mock_device_json(&MOCK_DEVICES[index]));
    }

    return ESP_OK;
}

esp_err_t ble_hid_bridge_scan_stop(void)
{
    state = "ready-stub";
    return ESP_OK;
}

esp_err_t ble_hid_bridge_pair_start(cJSON *params)
{
    const char *address = json_string(params, "address");
    const char *address_type = json_string(params, "addressType");
    const mock_hid_device_t *mock = find_mock_device(address);

    if (address[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    state = "pairing-stub";

    paired_device_t device = { 0 };
    make_id(device.id, sizeof(device.id), address);
    copy_text(device.address, sizeof(device.address), address);
    copy_text(device.address_type, sizeof(device.address_type), address_type[0] != '\0' ? address_type : "random");
    copy_text(device.name, sizeof(device.name), mock != NULL ? mock->name : address);
    copy_text(device.kind, sizeof(device.kind), mock != NULL ? mock->kind : "hid");
    device.auto_connect = true;
    device.connected = false;

    esp_err_t err = pairing_store_upsert(&device);
    if (err != ESP_OK) {
        state = "ready-stub";
        return err;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(pairing_store_set_connected(device.id, true));

    manager_protocol_emit_event("bond.changed", NULL);
    state = "ready-stub";
    ESP_LOGI(TAG, "mock paired %s", address);
    return ESP_OK;
}
