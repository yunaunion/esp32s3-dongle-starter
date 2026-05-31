#pragma once

#include "esp_err.h"

typedef enum {
    STATUS_LED_OFF = 0,
    STATUS_LED_BOOT,
    STATUS_LED_READY,
    STATUS_LED_SCANNING,
    STATUS_LED_PAIRING,
    STATUS_LED_CONNECTED,
    STATUS_LED_ERROR,
} status_led_state_t;

esp_err_t status_led_init(void);
void status_led_set(status_led_state_t state);
void status_led_activity(void);
