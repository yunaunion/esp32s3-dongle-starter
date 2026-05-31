#include "pairing_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define MAX_PAIRED_DEVICES 8

static const char *TAG = "pairing_store";
static const char *NVS_NAMESPACE = "dongle";
static const char *NVS_DEVICES_KEY = "devices";

static paired_device_t devices[MAX_PAIRED_DEVICES];
static size_t device_count;
static nvs_handle_t storage_handle;

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src != NULL ? src : "");
}

static const char *json_string(cJSON *object, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return "";
    }
    return item->valuestring;
}

static void load_device_from_json(cJSON *item, paired_device_t *device)
{
    memset(device, 0, sizeof(*device));
    copy_text(device->id, sizeof(device->id), json_string(item, "id"));
    copy_text(device->name, sizeof(device->name), json_string(item, "name"));
    copy_text(device->label, sizeof(device->label), json_string(item, "label"));
    copy_text(device->address, sizeof(device->address), json_string(item, "address"));
    copy_text(device->address_type, sizeof(device->address_type), json_string(item, "addressType"));
    copy_text(device->kind, sizeof(device->kind), json_string(item, "kind"));

    cJSON *auto_connect = cJSON_GetObjectItemCaseSensitive(item, "autoConnect");
    device->auto_connect = cJSON_IsBool(auto_connect) ? cJSON_IsTrue(auto_connect) : true;
    device->connected = false;
}

static cJSON *device_to_json(const paired_device_t *device)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", device->id);
    cJSON_AddStringToObject(item, "name", device->name);
    cJSON_AddStringToObject(item, "label", device->label);
    cJSON_AddStringToObject(item, "address", device->address);
    cJSON_AddStringToObject(item, "addressType", device->address_type);
    cJSON_AddStringToObject(item, "kind", device->kind);
    cJSON_AddBoolToObject(item, "autoConnect", device->auto_connect);
    return item;
}

static esp_err_t save_devices(void)
{
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < device_count; ++index) {
        cJSON_AddItemToArray(array, device_to_json(&devices[index]));
    }

    char *text = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_set_str(storage_handle, NVS_DEVICES_KEY, text);
    cJSON_free(text);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(storage_handle);
}

static esp_err_t load_devices(void)
{
    size_t required = 0;
    esp_err_t err = nvs_get_str(storage_handle, NVS_DEVICES_KEY, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        device_count = 0;
        return save_devices();
    }
    if (err != ESP_OK) {
        return err;
    }

    char *text = calloc(1, required);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(storage_handle, NVS_DEVICES_KEY, text, &required);
    if (err != ESP_OK) {
        free(text);
        return err;
    }

    cJSON *array = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(array);
        device_count = 0;
        return save_devices();
    }

    device_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (device_count >= MAX_PAIRED_DEVICES || !cJSON_IsObject(item)) {
            continue;
        }
        load_device_from_json(item, &devices[device_count]);
        if (devices[device_count].id[0] != '\0' && devices[device_count].address[0] != '\0') {
            ++device_count;
        }
    }
    cJSON_Delete(array);
    return ESP_OK;
}

static int find_index_by_id(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return -1;
    }
    for (size_t index = 0; index < device_count; ++index) {
        if (strcmp(devices[index].id, id) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int find_index_by_address(const char *address)
{
    if (address == NULL || address[0] == '\0') {
        return -1;
    }
    for (size_t index = 0; index < device_count; ++index) {
        if (strcmp(devices[index].address, address) == 0) {
            return (int)index;
        }
    }
    return -1;
}

esp_err_t pairing_store_init(void)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &storage_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = load_devices();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "pairing metadata store ready: %u device(s)", (unsigned)device_count);
    }
    return err;
}

size_t pairing_store_count(void)
{
    return device_count;
}

const paired_device_t *pairing_store_get(size_t index)
{
    if (index >= device_count) {
        return NULL;
    }
    return &devices[index];
}

const paired_device_t *pairing_store_find(const char *id)
{
    int index = find_index_by_id(id);
    if (index < 0) {
        return NULL;
    }
    return &devices[index];
}

esp_err_t pairing_store_upsert(const paired_device_t *device)
{
    if (device == NULL || device->id[0] == '\0' || device->address[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int index = find_index_by_id(device->id);
    if (index < 0) {
        index = find_index_by_address(device->address);
    }
    if (index < 0) {
        if (device_count >= MAX_PAIRED_DEVICES) {
            return ESP_ERR_NO_MEM;
        }
        index = (int)device_count++;
    }

    devices[index] = *device;
    return save_devices();
}

const paired_device_t *pairing_store_find_by_address(const char *address)
{
    int index = find_index_by_address(address);
    if (index < 0) {
        return NULL;
    }
    return &devices[index];
}

esp_err_t pairing_store_update_policy(const char *id, const char *label, const bool *auto_connect)
{
    int index = find_index_by_id(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (label != NULL) {
        copy_text(devices[index].label, sizeof(devices[index].label), label);
    }
    if (auto_connect != NULL) {
        devices[index].auto_connect = *auto_connect;
    }
    return save_devices();
}

esp_err_t pairing_store_set_connected(const char *id, bool connected)
{
    int index = find_index_by_id(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    devices[index].connected = connected;
    return ESP_OK;
}

esp_err_t pairing_store_delete(const char *id)
{
    int index = find_index_by_id(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t shift = (size_t)index; shift + 1 < device_count; ++shift) {
        devices[shift] = devices[shift + 1];
    }
    --device_count;
    ESP_LOGI(TAG, "deleted pairing metadata for %s", id);
    return save_devices();
}
