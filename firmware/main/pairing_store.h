#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    char id[32];
    char name[48];
    char label[48];
    char address[24];
    char address_type[12];
    char kind[16];
    bool auto_connect;
    bool connected;
} paired_device_t;

esp_err_t pairing_store_init(void);
size_t pairing_store_count(void);
const paired_device_t *pairing_store_get(size_t index);
const paired_device_t *pairing_store_find(const char *id);
esp_err_t pairing_store_upsert(const paired_device_t *device);
esp_err_t pairing_store_update_policy(const char *id, const char *label, const bool *auto_connect);
esp_err_t pairing_store_set_connected(const char *id, bool connected);
esp_err_t pairing_store_delete(const char *id);
