#include "manager_protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "ble_hid_bridge.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pairing_store.h"
#include "status_led.h"

#define FIRMWARE_VERSION "0.1.0"
#define PROTOCOL_VERSION 1

static const char *TAG = "manager";
static SemaphoreHandle_t s_stdout_mutex;
static manager_protocol_writer_t s_writer;

static void lock_stdout(void)
{
    if (s_stdout_mutex != NULL) {
        xSemaphoreTake(s_stdout_mutex, portMAX_DELAY);
    }
}

static void unlock_stdout(void)
{
    if (s_stdout_mutex != NULL) {
        xSemaphoreGive(s_stdout_mutex);
    }
}

static void send_json(cJSON *root)
{
    lock_stdout();
    char *text = cJSON_PrintUnformatted(root);
    if (text != NULL) {
        if (s_writer != NULL) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(s_writer(text, strlen(text)));
            ESP_ERROR_CHECK_WITHOUT_ABORT(s_writer("\n", 1));
        } else {
            printf("%s\n", text);
            fflush(stdout);
        }
        cJSON_free(text);
    }
    cJSON_Delete(root);
    unlock_stdout();
}

static void send_ok(const char *id, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data != NULL ? data : cJSON_CreateObject());
    send_json(root);
}

static void send_error(const char *id, const char *code, const char *message)
{
    status_led_set(STATUS_LED_ERROR);

    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(root, "error", error);
    send_json(root);
}

static bool any_device_connected(void)
{
    for (size_t index = 0; index < pairing_store_count(); ++index) {
        const paired_device_t *device = pairing_store_get(index);
        if (device != NULL && device->connected) {
            return true;
        }
    }
    return false;
}

static size_t connected_device_count(void)
{
    size_t connected = 0;
    for (size_t index = 0; index < pairing_store_count(); ++index) {
        const paired_device_t *device = pairing_store_get(index);
        if (device != NULL && device->connected) {
            ++connected;
        }
    }
    return connected;
}

static void show_store_led_state(void)
{
    status_led_set(any_device_connected() ? STATUS_LED_CONNECTED : STATUS_LED_READY);
}

void manager_protocol_init(void)
{
    if (s_stdout_mutex == NULL) {
        s_stdout_mutex = xSemaphoreCreateMutex();
    }
}

void manager_protocol_set_writer(manager_protocol_writer_t writer)
{
    s_writer = writer;
}

void manager_protocol_emit_event(const char *event, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddItemToObject(root, "data", data != NULL ? data : cJSON_CreateObject());
    send_json(root);
}

cJSON *manager_protocol_status_json(void)
{
    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(status, "ble", ble_hid_bridge_state());
    cJSON_AddStringToObject(status, "usb", "cdc+hid");
    cJSON_AddNumberToObject(status, "pairedCount", pairing_store_count());
    cJSON_AddNumberToObject(status, "connectedCount", connected_device_count());
    return status;
}

static cJSON *paired_devices_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *devices = cJSON_CreateArray();

    for (size_t index = 0; index < pairing_store_count(); ++index) {
        const paired_device_t *device = pairing_store_get(index);
        if (device == NULL) {
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", device->id);
        cJSON_AddStringToObject(item, "name", device->name);
        cJSON_AddStringToObject(item, "label", device->label);
        cJSON_AddStringToObject(item, "address", device->address);
        cJSON_AddStringToObject(item, "addressType", device->address_type);
        cJSON_AddStringToObject(item, "kind", device->kind);
        cJSON_AddBoolToObject(item, "autoConnect", device->auto_connect);
        cJSON_AddBoolToObject(item, "connected", device->connected);
        cJSON_AddItemToArray(devices, item);
    }

    cJSON_AddItemToObject(root, "devices", devices);
    return root;
}

static const char *json_string(cJSON *object, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return "";
    }
    return item->valuestring;
}

void manager_protocol_handle_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid JSON line");
        return;
    }

    const char *id = json_string(root, "id");
    const char *command = json_string(root, "command");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsObject(params)) {
        params = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "params", params);
    }

    if (id[0] == '\0' || command[0] == '\0') {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(command, "hello") == 0) {
        show_store_led_state();
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "protocol", PROTOCOL_VERSION);
        cJSON_AddStringToObject(data, "firmware", FIRMWARE_VERSION);
        send_ok(id, data);
    } else if (strcmp(command, "dongle.status") == 0) {
        send_ok(id, manager_protocol_status_json());
    } else if (strcmp(command, "bond.list") == 0) {
        send_ok(id, paired_devices_json());
    } else if (strcmp(command, "scan.start") == 0) {
        esp_err_t err = ble_hid_bridge_scan_start(params);
        if (err == ESP_OK) {
            status_led_set(STATUS_LED_SCANNING);
            cJSON *data = cJSON_CreateObject();
            cJSON_AddBoolToObject(data, "scanning", true);
            send_ok(id, data);
            manager_protocol_emit_event("status.changed", manager_protocol_status_json());
        } else {
            send_error(id, "scan_failed", "BLE HID scan could not be started");
        }
    } else if (strcmp(command, "scan.stop") == 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ble_hid_bridge_scan_stop());
        show_store_led_state();
        send_ok(id, NULL);
        manager_protocol_emit_event("status.changed", manager_protocol_status_json());
    } else if (strcmp(command, "pair.start") == 0) {
        esp_err_t err = ble_hid_bridge_pair_start(params);
        if (err == ESP_OK) {
            send_ok(id, NULL);
            manager_protocol_emit_event("status.changed", manager_protocol_status_json());
        } else {
            send_error(id, "pair_failed", "BLE HID pairing could not be completed");
        }
    } else if (strcmp(command, "bond.delete") == 0) {
        const char *device_id = json_string(params, "id");
        const paired_device_t *device = pairing_store_find(device_id);
        esp_err_t err = ESP_ERR_NOT_FOUND;
        if (device != NULL) {
            err = ble_hid_bridge_forget_device(device);
            if (err == ESP_OK) {
                err = pairing_store_delete(device_id);
            }
        }
        if (err == ESP_OK) {
            send_ok(id, NULL);
            manager_protocol_emit_event("bond.changed", paired_devices_json());
            manager_protocol_emit_event("status.changed", manager_protocol_status_json());
        } else {
            send_error(id, "not_found", "Device is not in the pairing store");
        }
    } else if (strcmp(command, "connect") == 0) {
        const char *device_id = json_string(params, "id");
        const paired_device_t *device = pairing_store_find(device_id);
        esp_err_t err = device != NULL ? ble_hid_bridge_connect_device(device) : ESP_ERR_NOT_FOUND;
        if (err == ESP_OK) {
            send_ok(id, NULL);
            manager_protocol_emit_event("status.changed", manager_protocol_status_json());
        } else {
            send_error(id, "not_found", "Device is not in the pairing store");
        }
    } else if (strcmp(command, "disconnect") == 0) {
        const char *device_id = json_string(params, "id");
        const paired_device_t *device = pairing_store_find(device_id);
        esp_err_t err = device != NULL ? ble_hid_bridge_disconnect_device(device) : ESP_ERR_NOT_FOUND;
        if (err == ESP_OK) {
            send_ok(id, NULL);
            manager_protocol_emit_event("status.changed", manager_protocol_status_json());
        } else {
            send_error(id, "not_found", "Device is not in the pairing store");
        }
    } else if (strcmp(command, "policy.set") == 0) {
        const char *device_id = json_string(params, "id");
        cJSON *auto_connect_item = cJSON_GetObjectItemCaseSensitive(params, "autoConnect");
        cJSON *label_item = cJSON_GetObjectItemCaseSensitive(params, "label");
        bool auto_connect = cJSON_IsTrue(auto_connect_item);
        const bool *auto_connect_ptr = cJSON_IsBool(auto_connect_item) ? &auto_connect : NULL;
        const char *label = cJSON_IsString(label_item) ? label_item->valuestring : NULL;
        esp_err_t err = pairing_store_update_policy(device_id, label, auto_connect_ptr);
        if (err == ESP_OK) {
            show_store_led_state();
            send_ok(id, NULL);
            manager_protocol_emit_event("bond.changed", paired_devices_json());
        } else {
            send_error(id, "not_found", "Device is not in the pairing store");
        }
    } else {
        send_error(id, "unknown_command", "Unknown command");
    }

    cJSON_Delete(root);
}
