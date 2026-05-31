#pragma once

#include "cJSON.h"
#include "esp_err.h"

esp_err_t ble_hid_bridge_init(void);
const char *ble_hid_bridge_state(void);
esp_err_t ble_hid_bridge_scan_start(cJSON *params);
esp_err_t ble_hid_bridge_scan_stop(void);
esp_err_t ble_hid_bridge_pair_start(cJSON *params);

