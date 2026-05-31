#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define STATUS_LED_GPIO GPIO_NUM_21
#define RMT_LED_RESOLUTION_HZ 10000000

static const char *TAG = "status_led";

static rmt_channel_handle_t led_channel;
static rmt_encoder_handle_t led_encoder;
static bool led_ready;

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
    status_led_set(STATUS_LED_BOOT);
    return ESP_OK;
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

void status_led_set(status_led_state_t state)
{
    switch (state) {
    case STATUS_LED_OFF:
        write_rgb(0, 0, 0);
        break;
    case STATUS_LED_BOOT:
        write_rgb(12, 12, 12);
        break;
    case STATUS_LED_READY:
        write_rgb(0, 0, 24);
        break;
    case STATUS_LED_SCANNING:
        write_rgb(0, 24, 24);
        break;
    case STATUS_LED_PAIRING:
        write_rgb(28, 0, 24);
        break;
    case STATUS_LED_CONNECTED:
        write_rgb(0, 28, 0);
        break;
    case STATUS_LED_ERROR:
        write_rgb(32, 0, 0);
        break;
    default:
        write_rgb(0, 0, 0);
        break;
    }
}
