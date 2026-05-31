#include "ble_hid_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"

#include "esp_hidh.h"
#include "status_led.h"

#include "manager_protocol.h"
#include "usb_hid_bridge.h"

extern void ble_store_config_init(void);

typedef struct {
    bool in_use;
    char address[24];
    esp_hidh_dev_t *dev;
} active_connection_t;

static const char *TAG = "ble_hid";
static const size_t MAX_DISCOVERY_NAME = 48;
static const size_t MAX_DISCOVERY_KIND = 16;
static const size_t MAX_DISCOVERY_ADDRESS = 24;
static const size_t MAX_DISCOVERY_ADDRESS_TYPE = 12;
static const uint32_t OPEN_MUTEX_TIMEOUT_MS = 10000;
static const uint32_t SCAN_CANCEL_WAIT_MS = 500;
static const uint32_t AUTO_RECONNECT_START_DELAY_MS = 1500;
static const uint32_t AUTO_RECONNECT_INTERVAL_MS = 6000;
static const uint32_t AUTO_RECONNECT_SCAN_MS = 3500;

static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_open_mutex;
static SemaphoreHandle_t s_sync_sem;
static bool s_scan_active;
static bool s_scan_hid_only = true;
static bool s_open_in_progress;
static bool s_auto_reconnect_started;
static char s_state[16] = "init";
static active_connection_t s_active_connections[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];

static const char *json_string(cJSON *object, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return "";
    }
    return item->valuestring;
}

static bool json_bool(cJSON *object, const char *key, bool default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_value;
}

static uint32_t json_u32(cJSON *object, const char *key, uint32_t default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        return (uint32_t)item->valuedouble;
    }
    return default_value;
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

    if (dst_size == 0) {
        return;
    }

    if (dst_size > 1) {
        dst[0] = '\0';
    }
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

static bool parse_bda(const char *address, uint8_t out[6])
{
    unsigned int values[6];

    if (address == NULL || out == NULL) {
        return false;
    }
    if (sscanf(address, "%2x:%2x:%2x:%2x:%2x:%2x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    for (size_t index = 0; index < 6; ++index) {
        out[index] = (uint8_t)values[index];
    }
    return true;
}

static void bda_to_string(const uint8_t *bda, char *dst, size_t dst_size)
{
    if (dst_size == 0) {
        return;
    }
    if (bda == NULL) {
        copy_text(dst, dst_size, "");
        return;
    }
    snprintf(dst, dst_size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static uint8_t address_type_from_text(const char *text)
{
    if (text != NULL && strcmp(text, "public") == 0) {
        return BLE_ADDR_PUBLIC;
    }
    return BLE_ADDR_RANDOM;
}

static const char *address_type_to_text(uint8_t type)
{
    if (type == BLE_ADDR_PUBLIC) {
        return "public";
    }
    return "random";
}

static void bytes_to_hex_string(const uint8_t *data, size_t length, char *out, size_t out_size)
{
    size_t max_bytes;
    size_t pos = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (data == NULL || length == 0) {
        return;
    }

    max_bytes = (out_size - 1) / 3;
    if (max_bytes > length) {
        max_bytes = length;
    }

    for (size_t index = 0; index < max_bytes; ++index) {
        int written = snprintf(out + pos, out_size - pos, "%02X", data[index]);
        if (written <= 0) {
            break;
        }
        pos += (size_t)written;
        if (index + 1 < max_bytes && pos + 1 < out_size) {
            out[pos++] = ' ';
            out[pos] = '\0';
        }
    }

    if (max_bytes < length && pos + 4 < out_size) {
        snprintf(out + pos, out_size - pos, " ...");
    }
}

static const char *kind_from_usage(esp_hid_usage_t usage)
{
    switch (usage) {
    case ESP_HID_USAGE_KEYBOARD:
        return "keyboard";
    case ESP_HID_USAGE_MOUSE:
        return "mouse";
    case ESP_HID_USAGE_JOYSTICK:
    case ESP_HID_USAGE_GAMEPAD:
        return "gamepad";
    default:
        return "hid";
    }
}

static bool any_device_connected_locked(void)
{
    for (size_t index = 0; index < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; ++index) {
        if (s_active_connections[index].in_use && s_active_connections[index].dev != NULL) {
            return true;
        }
    }
    return false;
}

static void set_state_locked(const char *state, status_led_state_t led)
{
    copy_text(s_state, sizeof(s_state), state);
    status_led_set(led);
}

static void update_state_after_transition(void)
{
    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }
    if (s_open_in_progress) {
        set_state_locked("pairing", STATUS_LED_PAIRING);
    } else {
        bool connected = any_device_connected_locked();
        set_state_locked(connected ? "connected" : "ready",
                         connected ? STATUS_LED_CONNECTED : STATUS_LED_READY);
    }
    if (s_state_mutex != NULL) {
        xSemaphoreGive(s_state_mutex);
    }
}

static void mark_connection_active(const char *address, esp_hidh_dev_t *dev)
{
    if (address == NULL || dev == NULL || s_state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t index = 0; index < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; ++index) {
        if (s_active_connections[index].in_use && strcmp(s_active_connections[index].address, address) == 0) {
            s_active_connections[index].dev = dev;
            xSemaphoreGive(s_state_mutex);
            return;
        }
    }

    for (size_t index = 0; index < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; ++index) {
        if (!s_active_connections[index].in_use) {
            s_active_connections[index].in_use = true;
            copy_text(s_active_connections[index].address, sizeof(s_active_connections[index].address), address);
            s_active_connections[index].dev = dev;
            xSemaphoreGive(s_state_mutex);
            return;
        }
    }

    s_active_connections[0].in_use = true;
    copy_text(s_active_connections[0].address, sizeof(s_active_connections[0].address), address);
    s_active_connections[0].dev = dev;
    xSemaphoreGive(s_state_mutex);
}

static void clear_connection_active(const char *address)
{
    if (address == NULL || s_state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t index = 0; index < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; ++index) {
        if (s_active_connections[index].in_use && strcmp(s_active_connections[index].address, address) == 0) {
            s_active_connections[index].in_use = false;
            s_active_connections[index].address[0] = '\0';
            s_active_connections[index].dev = NULL;
        }
    }
    xSemaphoreGive(s_state_mutex);
}

static esp_hidh_dev_t *find_active_connection(const char *address)
{
    esp_hidh_dev_t *dev = NULL;

    if (address == NULL || s_state_mutex == NULL) {
        return NULL;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t index = 0; index < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; ++index) {
        if (s_active_connections[index].in_use && strcmp(s_active_connections[index].address, address) == 0) {
            dev = s_active_connections[index].dev;
            break;
        }
    }
    xSemaphoreGive(s_state_mutex);
    return dev;
}

static bool is_hid_candidate(const struct ble_hs_adv_fields *fields)
{
    if (fields == NULL) {
        return false;
    }

    if (fields->appearance_is_present) {
        esp_hid_usage_t usage = esp_hid_usage_from_appearance(fields->appearance);
        if (usage != ESP_HID_USAGE_GENERIC) {
            return true;
        }
    }

    for (int index = 0; index < fields->num_uuids16; ++index) {
        if (ble_uuid_u16(&fields->uuids16[index].u) == 0x1812) {
            return true;
        }
    }

    return false;
}

static void emit_status_changed(void)
{
    manager_protocol_emit_event("status.changed", manager_protocol_status_json());
}

static void emit_device_discovered(const char *name, const char *address, const char *address_type,
                                   const char *kind, int rssi)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "name", name != NULL && name[0] != '\0' ? name : address);
    cJSON_AddStringToObject(data, "address", address);
    cJSON_AddStringToObject(data, "addressType", address_type);
    cJSON_AddStringToObject(data, "kind", kind);
    cJSON_AddNumberToObject(data, "rssi", rssi);
    manager_protocol_emit_event("device.discovered", data);
}

static bool open_in_progress(void)
{
    bool in_progress = false;
    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        in_progress = s_open_in_progress;
        xSemaphoreGive(s_state_mutex);
    }
    return in_progress;
}

static bool scan_active(void)
{
    bool active = false;
    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        active = s_scan_active;
        xSemaphoreGive(s_state_mutex);
    }
    return active;
}

static void cancel_scan_for_connection(void)
{
    if (ble_gap_disc_active()) {
        int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "ble_gap_disc_cancel before open failed: %d", rc);
        }

        for (uint32_t elapsed = 0; elapsed < SCAN_CANCEL_WAIT_MS && ble_gap_disc_active(); elapsed += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_scan_active = false;
        xSemaphoreGive(s_state_mutex);
    } else {
        s_scan_active = false;
    }
}

static void disable_auto_connect_after_failure(const paired_device_t *device)
{
    if (device == NULL || device->id[0] == '\0' || pairing_store_find(device->id) == NULL) {
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(pairing_store_set_connected(device->id, false));
    manager_protocol_emit_event("bond.changed", NULL);
}

static esp_err_t open_device_blocking(const paired_device_t *device, bool store_on_success)
{
    uint8_t bda[6];
    uint8_t addr_type;
    esp_hidh_dev_t *dev = NULL;
    esp_err_t err = ESP_OK;

    if (device == NULL || device->address[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_open_mutex == NULL || xSemaphoreTake(s_open_mutex, pdMS_TO_TICKS(OPEN_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    if (find_active_connection(device->address) != NULL) {
        if (device->id[0] != '\0') {
            ESP_ERROR_CHECK_WITHOUT_ABORT(pairing_store_set_connected(device->id, true));
        }
        manager_protocol_emit_event("bond.changed", NULL);
        xSemaphoreGive(s_open_mutex);
        return ESP_OK;
    }

    if (!parse_bda(device->address, bda)) {
        xSemaphoreGive(s_open_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    addr_type = address_type_from_text(device->address_type);

    cancel_scan_for_connection();

    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_open_in_progress = true;
        set_state_locked("pairing", STATUS_LED_PAIRING);
        xSemaphoreGive(s_state_mutex);
    } else {
        s_open_in_progress = true;
        copy_text(s_state, sizeof(s_state), "pairing");
        status_led_set(STATUS_LED_PAIRING);
    }
    emit_status_changed();

    dev = esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE, addr_type);
    if (dev == NULL) {
        disable_auto_connect_after_failure(device);
        if (s_state_mutex != NULL) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_open_in_progress = false;
            xSemaphoreGive(s_state_mutex);
        } else {
            s_open_in_progress = false;
        }
        update_state_after_transition();
        emit_status_changed();
        xSemaphoreGive(s_open_mutex);
        return ESP_FAIL;
    }

    if (store_on_success) {
        err = pairing_store_upsert(device);
        if (err != ESP_OK) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_hidh_dev_close(dev));
            if (s_state_mutex != NULL) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_open_in_progress = false;
                xSemaphoreGive(s_state_mutex);
            } else {
                s_open_in_progress = false;
            }
            update_state_after_transition();
            emit_status_changed();
            xSemaphoreGive(s_open_mutex);
            return err;
        }
    }

    mark_connection_active(device->address, dev);
    if (device->id[0] != '\0') {
        ESP_ERROR_CHECK_WITHOUT_ABORT(pairing_store_set_connected(device->id, true));
    }
    manager_protocol_emit_event("bond.changed", NULL);
    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_open_in_progress = false;
        xSemaphoreGive(s_state_mutex);
    } else {
        s_open_in_progress = false;
    }
    update_state_after_transition();
    emit_status_changed();
    xSemaphoreGive(s_open_mutex);
    return ESP_OK;
}

static void auto_connect_task(void *arg)
{
    paired_device_t *device = (paired_device_t *)arg;

    (void)open_device_blocking(device, true);

    free(device);
    vTaskDelete(NULL);
}

static esp_err_t schedule_auto_connect(const paired_device_t *device)
{
    paired_device_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *ctx = *device;
    ctx->auto_connect = true;
    ctx->connected = false;

    if (xTaskCreate(auto_connect_task, "ble_auto_connect", 8192, ctx, 4, NULL) != pdPASS) {
        free(ctx);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static const paired_device_t *find_auto_connect_record(const char *name, const char *address, const char *kind)
{
    const paired_device_t *record = pairing_store_find_by_address(address);
    if (record != NULL) {
        return record;
    }

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    for (size_t index = 0; index < pairing_store_count(); ++index) {
        const paired_device_t *candidate = pairing_store_get(index);
        if (candidate == NULL || !candidate->auto_connect || candidate->connected) {
            continue;
        }
        if (candidate->name[0] == '\0' || strcmp(candidate->name, name) != 0) {
            continue;
        }
        if (kind != NULL && kind[0] != '\0' &&
                candidate->kind[0] != '\0' && strcmp(candidate->kind, kind) != 0) {
            continue;
        }
        return candidate;
    }

    return NULL;
}

static void maybe_auto_connect(const char *name, const char *address, const char *address_type, const char *kind)
{
    const paired_device_t *record = find_auto_connect_record(name, address, kind);
    if (record == NULL || !record->auto_connect || record->connected || open_in_progress()) {
        return;
    }

    paired_device_t request = *record;
    copy_text(request.address, sizeof(request.address), address);
    copy_text(request.address_type, sizeof(request.address_type), address_type);
    if (name != NULL && name[0] != '\0') {
        copy_text(request.name, sizeof(request.name), name);
    }
    if (kind != NULL && kind[0] != '\0') {
        copy_text(request.kind, sizeof(request.kind), kind);
    }

    (void)schedule_auto_connect(&request);
}

static bool has_auto_connect_target(void)
{
    for (size_t index = 0; index < pairing_store_count(); ++index) {
        const paired_device_t *device = pairing_store_get(index);
        if (device == NULL || !device->auto_connect || device->connected) {
            continue;
        }
        if (find_active_connection(device->address) == NULL) {
            return true;
        }
    }
    return false;
}

static void auto_reconnect_scan_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(AUTO_RECONNECT_START_DELAY_MS));

    while (true) {
        if (has_auto_connect_target() && !open_in_progress() && !scan_active()) {
            cJSON *params = cJSON_CreateObject();
            if (params != NULL) {
                cJSON_AddNumberToObject(params, "durationMs", AUTO_RECONNECT_SCAN_MS);
                cJSON_AddBoolToObject(params, "hidOnly", true);
                ESP_ERROR_CHECK_WITHOUT_ABORT(ble_hid_bridge_scan_start(params));
                cJSON_Delete(params);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(AUTO_RECONNECT_INTERVAL_MS));
    }
}

static int scan_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        char address[MAX_DISCOVERY_ADDRESS];
        const char *name = NULL;
        const char *kind = "hid";
        char safe_name[MAX_DISCOVERY_NAME];
        char safe_kind[MAX_DISCOVERY_KIND];
        char address_type[MAX_DISCOVERY_ADDRESS_TYPE];
        esp_hid_usage_t usage = ESP_HID_USAGE_GENERIC;
        bool hid_candidate = false;

        if (open_in_progress()) {
            return 0;
        }

        memset(&fields, 0, sizeof(fields));
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }

        bda_to_string(event->disc.addr.val, address, sizeof(address));
        copy_text(address_type, sizeof(address_type), address_type_to_text(event->disc.addr.type));

        if (fields.name != NULL && fields.name_len > 0) {
            size_t name_len = fields.name_len < sizeof(safe_name) - 1 ? fields.name_len : sizeof(safe_name) - 1;
            memcpy(safe_name, fields.name, name_len);
            safe_name[name_len] = '\0';
            name = safe_name;
        }

        if (fields.appearance_is_present) {
            usage = esp_hid_usage_from_appearance(fields.appearance);
        }
        if (usage != ESP_HID_USAGE_GENERIC) {
            kind = kind_from_usage(usage);
        }
        copy_text(safe_kind, sizeof(safe_kind), kind);

        hid_candidate = is_hid_candidate(&fields);
        if (s_scan_hid_only && !hid_candidate) {
            return 0;
        }

        emit_device_discovered(name != NULL ? name : address, address, address_type, safe_kind, event->disc.rssi);
        maybe_auto_connect(name != NULL ? name : address, address, address_type, safe_kind);
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE: {
        if (s_state_mutex != NULL) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_scan_active = false;
            if (s_open_in_progress) {
                set_state_locked("pairing", STATUS_LED_PAIRING);
            } else {
                bool connected = any_device_connected_locked();
                set_state_locked(connected ? "connected" : "ready",
                                 connected ? STATUS_LED_CONNECTED : STATUS_LED_READY);
            }
            xSemaphoreGive(s_state_mutex);
        } else {
            s_scan_active = false;
            update_state_after_transition();
        }
        emit_status_changed();
        break;
    }
    default:
        break;
    }

    return 0;
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void nimble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset: %d", reason);
}

static void nimble_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
    }

    if (s_sync_sem != NULL) {
        xSemaphoreGive(s_sync_sem);
    }
}

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;
    const uint8_t *bda = NULL;
    char address[MAX_DISCOVERY_ADDRESS];
    const char *name = "";

    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        if (param->open.dev != NULL && param->open.status == ESP_OK) {
            bda = esp_hidh_dev_bda_get(param->open.dev);
            bda_to_string(bda, address, sizeof(address));
            name = esp_hidh_dev_name_get(param->open.dev);
            ESP_LOGI(TAG, "OPEN %s %s", address, name != NULL ? name : "");
            mark_connection_active(address, param->open.dev);

            const paired_device_t *record = pairing_store_find_by_address(address);
            if (record != NULL) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(pairing_store_set_connected(record->id, true));
            }

            manager_protocol_emit_event("bond.changed", NULL);
            update_state_after_transition();
            emit_status_changed();
        }
        break;

    case ESP_HIDH_CLOSE_EVENT:
        if (param->close.dev != NULL) {
            bda = esp_hidh_dev_bda_get(param->close.dev);
            bda_to_string(bda, address, sizeof(address));
            name = esp_hidh_dev_name_get(param->close.dev);
            ESP_LOGI(TAG, "CLOSE %s %s reason=%d", address, name != NULL ? name : "", param->close.reason);
            clear_connection_active(address);

            const paired_device_t *record = pairing_store_find_by_address(address);
            if (record != NULL) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(pairing_store_set_connected(record->id, false));
            }

            manager_protocol_emit_event("bond.changed", NULL);
            update_state_after_transition();
            emit_status_changed();
        }
        break;

    case ESP_HIDH_INPUT_EVENT:
        if (param->input.dev != NULL) {
            esp_err_t forward_err;
            char raw_hex[64] = { 0 };
            bda = esp_hidh_dev_bda_get(param->input.dev);
            bda_to_string(bda, address, sizeof(address));
            ESP_LOGI(TAG, "INPUT %s usage=%s map=%u id=%u len=%u", address,
                     esp_hid_usage_str(param->input.usage),
                     (unsigned)param->input.map_index,
                     (unsigned)param->input.report_id,
                     (unsigned)param->input.length);
            forward_err = usb_hid_bridge_forward_input(param->input.usage,
                          param->input.report_id,
                          param->input.data,
                          param->input.length);

            if (forward_err == ESP_ERR_NOT_SUPPORTED) {
                /* Some BLE mice/controllers are reported as GENERIC usage. */
                if (param->input.length >= 3) {
                    forward_err = usb_hid_bridge_forward_input(ESP_HID_USAGE_MOUSE,
                                  param->input.report_id,
                                  param->input.data,
                                  param->input.length);
                    if (forward_err == ESP_OK) {
                        ESP_LOGW(TAG, "INPUT fallback as mouse applied for %s", address);
                    }
                }
            }

            if (forward_err != ESP_OK) {
                ESP_LOGW(TAG, "INPUT forward failed (%s) usage=%s len=%u", esp_err_to_name(forward_err),
                         esp_hid_usage_str(param->input.usage), (unsigned)param->input.length);
            } else {
                status_led_activity();
            }

            cJSON *debug = cJSON_CreateObject();
            if (debug != NULL) {
                bytes_to_hex_string(param->input.data, param->input.length, raw_hex, sizeof(raw_hex));
                cJSON_AddStringToObject(debug, "address", address);
                cJSON_AddStringToObject(debug, "usage", esp_hid_usage_str(param->input.usage));
                cJSON_AddNumberToObject(debug, "reportId", (double)param->input.report_id);
                cJSON_AddNumberToObject(debug, "length", (double)param->input.length);
                cJSON_AddStringToObject(debug, "raw", raw_hex);
                cJSON_AddStringToObject(debug, "forward", esp_err_to_name(forward_err));
                manager_protocol_emit_event("input.debug", debug);
            }
        }
        break;

    case ESP_HIDH_BATTERY_EVENT:
        if (param->battery.dev != NULL) {
            bda = esp_hidh_dev_bda_get(param->battery.dev);
            bda_to_string(bda, address, sizeof(address));
            ESP_LOGI(TAG, "BATTERY %s level=%u%%", address, (unsigned)param->battery.level);
        }
        break;

    case ESP_HIDH_FEATURE_EVENT:
        if (param->feature.dev != NULL) {
            bda = esp_hidh_dev_bda_get(param->feature.dev);
            bda_to_string(bda, address, sizeof(address));
            ESP_LOGI(TAG, "FEATURE %s usage=%s map=%u id=%u len=%u", address,
                     esp_hid_usage_str(param->feature.usage),
                     (unsigned)param->feature.map_index,
                     (unsigned)param->feature.report_id,
                     (unsigned)param->feature.length);
        }
        break;

    default:
        break;
    }
}

static paired_device_t paired_device_from_params(cJSON *params)
{
    paired_device_t device = { 0 };
    const char *address = json_string(params, "address");
    const char *address_type = json_string(params, "addressType");
    const char *name = json_string(params, "name");
    const char *kind = json_string(params, "kind");
    const char *label = json_string(params, "label");
    const paired_device_t *existing = NULL;

    make_id(device.id, sizeof(device.id), address);
    copy_text(device.address, sizeof(device.address), address);
    copy_text(device.address_type, sizeof(device.address_type), address_type[0] != '\0' ? address_type : "random");
    copy_text(device.name, sizeof(device.name), name[0] != '\0' ? name : address);
    copy_text(device.kind, sizeof(device.kind), kind[0] != '\0' ? kind : "hid");
    copy_text(device.label, sizeof(device.label), label);
    device.auto_connect = true;
    device.connected = false;

    existing = pairing_store_find(device.id);
    if (existing != NULL) {
        if (device.label[0] == '\0') {
            copy_text(device.label, sizeof(device.label), existing->label);
        }
        device.auto_connect = existing->auto_connect;
        device.connected = existing->connected;
    }

    return device;
}

esp_err_t ble_hid_bridge_init(void)
{
    esp_err_t err;
    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 8192,
        .callback_arg = NULL,
    };

    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    if (s_open_mutex == NULL) {
        s_open_mutex = xSemaphoreCreateMutex();
    }
    if (s_sync_sem == NULL) {
        s_sync_sem = xSemaphoreCreateBinary();
    }
    if (s_state_mutex == NULL || s_open_mutex == NULL || s_sync_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_hidh_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed: %s", esp_err_to_name(err));
        nimble_port_deinit();
        return err;
    }

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.reset_cb = nimble_on_reset;
    ble_hs_cfg.sync_cb = nimble_on_sync;

    err = esp_nimble_enable(nimble_host_task);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_enable failed: %s", esp_err_to_name(err));
        nimble_port_deinit();
        return err;
    }

    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "NimBLE sync timed out");
        nimble_port_deinit();
        return ESP_ERR_TIMEOUT;
    }

    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_scan_active = false;
        s_open_in_progress = false;
        set_state_locked("ready", STATUS_LED_READY);
        xSemaphoreGive(s_state_mutex);
    } else {
        copy_text(s_state, sizeof(s_state), "ready");
        status_led_set(STATUS_LED_READY);
    }

    ESP_LOGI(TAG, "BLE HID bridge ready");
    if (!s_auto_reconnect_started) {
        ESP_RETURN_ON_FALSE(
            xTaskCreate(auto_reconnect_scan_task, "ble_reconnect", 8192, NULL, 3, NULL) == pdPASS,
            ESP_ERR_NO_MEM, TAG, "Auto reconnect task create failed");
        s_auto_reconnect_started = true;
    }
    return ESP_OK;
}

const char *ble_hid_bridge_state(void)
{
    return s_state;
}

esp_err_t ble_hid_bridge_scan_start(cJSON *params)
{
    uint32_t duration_ms;
    uint8_t own_addr_type = BLE_ADDR_PUBLIC;
    struct ble_gap_disc_params disc_params = { 0 };
    int rc;

    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (s_scan_active) {
            xSemaphoreGive(s_state_mutex);
            return ESP_OK;
        }
        s_scan_active = true;
        set_state_locked("scanning", STATUS_LED_SCANNING);
        xSemaphoreGive(s_state_mutex);
    } else {
        s_scan_active = true;
        copy_text(s_state, sizeof(s_state), "scanning");
        status_led_set(STATUS_LED_SCANNING);
    }

    emit_status_changed();

    duration_ms = json_u32(params, "durationMs", 5000);
    s_scan_hid_only = json_bool(params, "hidOnly", true);

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        s_scan_active = false;
        update_state_after_transition();
        emit_status_changed();
        return ESP_FAIL;
    }

    disc_params.filter_duplicates = 1;
    disc_params.passive = 0;
    disc_params.itvl = 0x50;
    disc_params.window = 0x30;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    rc = ble_gap_disc(own_addr_type, (int32_t)duration_ms, &disc_params, scan_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        s_scan_active = false;
        update_state_after_transition();
        emit_status_changed();
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ble_hid_bridge_scan_stop(void)
{
    int rc = ble_gap_disc_cancel();

    if (rc != 0) {
        if (rc == BLE_HS_EALREADY) {
            s_scan_active = false;
            update_state_after_transition();
            emit_status_changed();
            return ESP_OK;
        }
        ESP_LOGE(TAG, "ble_gap_disc_cancel failed: %d", rc);
        return ESP_FAIL;
    }

    s_scan_active = false;
    update_state_after_transition();
    emit_status_changed();
    return ESP_OK;
}

esp_err_t ble_hid_bridge_pair_start(cJSON *params)
{
    paired_device_t device = paired_device_from_params(params);
    esp_err_t err;

    if (device.address[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = open_device_blocking(&device, true);
    if (err != ESP_OK) {
        update_state_after_transition();
        emit_status_changed();
        return err;
    }

    return ESP_OK;
}

esp_err_t ble_hid_bridge_connect_device(const paired_device_t *device)
{
    esp_err_t err;

    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = open_device_blocking(device, false);
    if (err != ESP_OK) {
        update_state_after_transition();
        emit_status_changed();
        return err;
    }

    return ESP_OK;
}

esp_err_t ble_hid_bridge_disconnect_device(const paired_device_t *device)
{
    esp_hidh_dev_t *dev = NULL;

    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dev = find_active_connection(device->address);
    if (dev == NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(pairing_store_set_connected(device->id, false));
        update_state_after_transition();
        emit_status_changed();
        return ESP_OK;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_hidh_dev_close(dev));
    ESP_ERROR_CHECK_WITHOUT_ABORT(pairing_store_set_connected(device->id, false));
    clear_connection_active(device->address);
    update_state_after_transition();
    emit_status_changed();
    return ESP_OK;
}

esp_err_t ble_hid_bridge_forget_device(const paired_device_t *device)
{
    ble_addr_t peer = { 0 };
    esp_hidh_dev_t *active = NULL;

    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    active = find_active_connection(device->address);
    if (active != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_hidh_dev_close(active));
    }

    peer.type = address_type_from_text(device->address_type);
    if (parse_bda(device->address, peer.val)) {
        int rc = ble_gap_unpair(&peer);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gap_unpair failed for %s: %d", device->address, rc);
        }
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(pairing_store_set_connected(device->id, false));
    clear_connection_active(device->address);
    update_state_after_transition();
    emit_status_changed();
    return ESP_OK;
}
