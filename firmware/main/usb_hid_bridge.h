#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t usb_hid_bridge_init(void);
int usb_hid_bridge_read_byte(TickType_t timeout_ticks);
esp_err_t usb_hid_bridge_write(const char *data, size_t length);
esp_err_t usb_hid_bridge_forward_input(int usage, uint16_t report_id,
                                       const uint8_t *data, size_t length);
