#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "pairing_store.h"

esp_err_t ble_hid_bridge_init(void);
const char *ble_hid_bridge_state(void);
esp_err_t ble_hid_bridge_scan_start(cJSON *params);
esp_err_t ble_hid_bridge_scan_stop(void);
esp_err_t ble_hid_bridge_pair_start(cJSON *params);
esp_err_t ble_hid_bridge_connect_device(const paired_device_t *device);
esp_err_t ble_hid_bridge_disconnect_device(const paired_device_t *device);
esp_err_t ble_hid_bridge_forget_device(const paired_device_t *device);
