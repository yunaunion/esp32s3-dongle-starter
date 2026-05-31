#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STATUS_LED_GPIO GPIO_NUM_21
#define RMT_LED_RESOLUTION_HZ 10000000

static const char *TAG = "status_led";

static rmt_channel_handle_t led_channel;
static rmt_encoder_handle_t led_encoder;
static bool led_ready;
static volatile status_led_state_t s_target_state = STATUS_LED_OFF;
static volatile TickType_t s_activity_until_ticks = 0;
static TaskHandle_t s_led_task;

static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 3,
    .level1 = 0,
    .duration1 = 9,
};

static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 9,
    .level1 = 0,
    .duration1 = 3,
};

static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = 250,
    .level1 = 0,
    .duration1 = 250,
};

static size_t ws2812_encoder_callback(const void *data, size_t data_size,
                                      size_t symbols_written, size_t symbols_free,
                                      rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;

    if (symbols_free < 8) {
        return 0;
    }

    size_t data_pos = symbols_written / 8;
    const uint8_t *bytes = (const uint8_t *)data;
    if (data_pos < data_size) {
        size_t symbol_pos = 0;
        for (uint8_t bitmask = 0x80; bitmask != 0; bitmask >>= 1) {
            symbols[symbol_pos++] = (bytes[data_pos] & bitmask) ? ws2812_one : ws2812_zero;
        }
        return symbol_pos;
    }

    symbols[0] = ws2812_reset;
    *done = true;
    return 1;
}

static void write_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!led_ready) {
        return;
    }

    const uint8_t rgb[3] = { red, green, blue };
    const rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(led_channel, led_encoder, rgb, sizeof(rgb), &tx_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(led_channel, 100);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED update failed: %s", esp_err_to_name(err));
    }
    esp_rom_delay_us(80);
}

static void led_task(void *arg)
{
    TickType_t tick = 0;
    (void)arg;

    while (true) {
        status_led_state_t state = s_target_state;
        bool activity = xTaskGetTickCount() < s_activity_until_ticks;
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;

        switch (state) {
        case STATUS_LED_OFF:
            r = 0; g = 0; b = 0;
            break;
        case STATUS_LED_BOOT:
            /* White slow blink while firmware is booting. */
            if ((tick / 5) % 2 == 0) {
                r = 10; g = 10; b = 10;
            }
            break;
        case STATUS_LED_READY:
            /* Blue heartbeat. */
            if ((tick % 20) == 0 || (tick % 20) == 1) {
                r = 0; g = 8; b = 32;
            } else {
                r = 0; g = 0; b = 16;
            }
            break;
        case STATUS_LED_SCANNING:
            /* Cyan fast blink while scanning. */
            if ((tick / 2) % 2 == 0) {
                r = 0; g = 26; b = 26;
            } else {
                r = 0; g = 4; b = 4;
            }
            break;
        case STATUS_LED_PAIRING:
            /* Magenta pulse while pairing / connecting. */
            if ((tick / 2) % 2 == 0) {
                r = 30; g = 0; b = 20;
            } else {
                r = 8; g = 0; b = 6;
            }
            break;
        case STATUS_LED_CONNECTED:
            /* Green solid, brief brighter flash on input activity. */
            if (activity) {
                r = 8; g = 40; b = 8;
            } else {
                r = 0; g = 26; b = 0;
            }
            break;
        case STATUS_LED_ERROR:
            /* Red double-blink. */
            if ((tick % 16) == 0 || (tick % 16) == 1 || (tick % 16) == 4 || (tick % 16) == 5) {
                r = 34; g = 0; b = 0;
            } else {
                r = 2; g = 0; b = 0;
            }
            break;
        default:
            break;
        }

        write_rgb(r, g, b);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void status_led_set(status_led_state_t state)
{
    s_target_state = state;
}

void status_led_activity(void)
{
    s_activity_until_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(180);
}

esp_err_t status_led_init(void)
{
    if (led_ready) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = STATUS_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_LED_RESOLUTION_HZ,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_config, &led_channel), TAG, "RMT TX channel init failed");

    rmt_simple_encoder_config_t encoder_config = {
        .callback = ws2812_encoder_callback,
    };
    ESP_RETURN_ON_ERROR(rmt_new_simple_encoder(&encoder_config, &led_encoder), TAG, "RMT encoder init failed");
    ESP_RETURN_ON_ERROR(rmt_enable(led_channel), TAG, "RMT TX channel enable failed");

    led_ready = true;
    s_target_state = STATUS_LED_BOOT;
    ESP_RETURN_ON_FALSE(
        xTaskCreate(led_task, "status_led", 3072, NULL, tskIDLE_PRIORITY + 1, &s_led_task) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "LED task create failed");

    return ESP_OK;
}
