#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_hid_bridge.h"
#include "manager_protocol.h"
#include "pairing_store.h"
#include "status_led.h"
#include "usb_hid_bridge.h"

static const char *TAG = "dongle";

static void manager_task(void *arg)
{
    char line[512];
    size_t line_len = 0;

    while (true) {
        int ch = usb_hid_bridge_read_byte(pdMS_TO_TICKS(10));
        if (ch < 0) {
            continue;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            line[line_len] = '\0';
            if (line_len > 0) {
                manager_protocol_handle_line(line);
            }
            line_len = 0;
            continue;
        }

        if (line_len + 1 < sizeof(line)) {
            line[line_len++] = (char)ch;
        } else {
            line_len = 0;
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(status_led_init());
    status_led_set(STATUS_LED_BOOT);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(pairing_store_init());
    ESP_ERROR_CHECK(usb_hid_bridge_init());
    manager_protocol_set_writer(usb_hid_bridge_write);
    ESP_ERROR_CHECK(ble_hid_bridge_init());
    manager_protocol_init();

    status_led_set(STATUS_LED_READY);
    manager_protocol_emit_event("status.changed", manager_protocol_status_json());

    xTaskCreate(manager_task, "manager_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "ESP32-S3 BLE HID dongle manager started");
}
