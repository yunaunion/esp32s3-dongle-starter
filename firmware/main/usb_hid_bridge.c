#include "usb_hid_bridge.h"

#include <stdbool.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "class/cdc/cdc_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_hid";

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE 2
#define REPORT_ID_CONSUMER 3
#define REPORT_ID_GAMEPAD 4

#define ITF_NUM_CDC 0
#define ITF_NUM_CDC_DATA 1
#define ITF_NUM_HID 2
#define ITF_NUM_TOTAL 3

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define EPNUM_HID_IN 0x83

#define USB_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#define HID_READY_POLL_MS 1
#define HID_SEND_RETRY_COUNT 12
#define HID_SEND_RETRY_DELAY_MS 1

static QueueHandle_t s_cdc_rx_queue;
static QueueHandle_t s_hid_tx_queue;
static uint8_t s_cdc_rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
static bool s_driver_ready;

typedef enum {
    HID_TX_KEYBOARD = 0,
    HID_TX_MOUSE,
    HID_TX_REPORT,
} hid_tx_kind_t;

typedef struct {
    hid_tx_kind_t kind;
    uint8_t report_id;
    uint8_t modifier;
    uint8_t keycodes[6];
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
    int8_t pan;
    uint8_t report[16];
    uint16_t report_len;
} hid_tx_item_t;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER)),
    TUD_HID_REPORT_DESC_GAMEPAD(HID_REPORT_ID(REPORT_ID_GAMEPAD)),
};

static const char *s_string_descriptor[] = {
    (const char[]){ 0x09, 0x04 },
    "YunaUnion",
    "ESP32-S3 BLE HID Dongle",
    "000001",
    "Management CDC",
    "Keyboard Mouse Gamepad HID",
};

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, USB_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 5, false, sizeof(s_hid_report_descriptor),
                       EPNUM_HID_IN, 16, 10),
};

#include "esp_hid_common.h"

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

static bool wait_hid_ready(uint32_t timeout_ms);
static esp_err_t wait_and_send_keyboard(uint8_t modifier, uint8_t keycodes[6]);
static esp_err_t wait_and_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel, int8_t pan);
static esp_err_t wait_and_send_report(uint8_t report_id, const void *report, uint16_t len);

static void hid_tx_task(void *arg)
{
    hid_tx_item_t item;
    (void)arg;

    while (true) {
        if (xQueueReceive(s_hid_tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (item.kind) {
        case HID_TX_KEYBOARD:
            ESP_ERROR_CHECK_WITHOUT_ABORT(wait_and_send_keyboard(item.modifier, item.keycodes));
            break;
        case HID_TX_MOUSE:
            ESP_ERROR_CHECK_WITHOUT_ABORT(wait_and_send_mouse(item.buttons, item.x, item.y, item.wheel, item.pan));
            break;
        case HID_TX_REPORT:
            ESP_ERROR_CHECK_WITHOUT_ABORT(wait_and_send_report(item.report_id, item.report, item.report_len));
            break;
        default:
            break;
        }
    }
}

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;

    size_t rx_size = 0;
    esp_err_t err = tinyusb_cdcacm_read(itf, s_cdc_rx_buf, sizeof(s_cdc_rx_buf), &rx_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CDC read failed: %s", esp_err_to_name(err));
        return;
    }

    for (size_t index = 0; index < rx_size; ++index) {
        (void)xQueueSend(s_cdc_rx_queue, &s_cdc_rx_buf[index], 0);
    }
}

esp_err_t usb_hid_bridge_init(void)
{
    if (s_driver_ready) {
        return ESP_OK;
    }

    s_cdc_rx_queue = xQueueCreate(2048, sizeof(uint8_t));
    ESP_RETURN_ON_FALSE(s_cdc_rx_queue != NULL, ESP_ERR_NO_MEM, TAG, "CDC RX queue allocation failed");
    s_hid_tx_queue = xQueueCreate(128, sizeof(hid_tx_item_t));
    ESP_RETURN_ON_FALSE(s_hid_tx_queue != NULL, ESP_ERR_NO_MEM, TAG, "HID TX queue allocation failed");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = s_configuration_descriptor;
    tusb_cfg.descriptor.string = s_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_configuration_descriptor;
#endif

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB driver install failed");

    tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&cdc_cfg), TAG, "TinyUSB CDC init failed");

    s_driver_ready = true;
    ESP_RETURN_ON_FALSE(
        xTaskCreate(hid_tx_task, "hid_tx", 4096, NULL, tskIDLE_PRIORITY + 3, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "HID TX task create failed");
    ESP_LOGI(TAG, "TinyUSB CDC + HID bridge initialized");
    return ESP_OK;
}

int usb_hid_bridge_read_byte(TickType_t timeout_ticks)
{
    uint8_t byte = 0;
    if (s_cdc_rx_queue == NULL || xQueueReceive(s_cdc_rx_queue, &byte, timeout_ticks) != pdTRUE) {
        return -1;
    }
    return byte;
}

esp_err_t usb_hid_bridge_write(const char *data, size_t length)
{
    if (!s_driver_ready || data == NULL || length == 0) {
        return ESP_OK;
    }
    if (!tud_cdc_n_connected(TINYUSB_CDC_ACM_0)) {
        return ESP_OK;
    }

    size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)data, length);
    if (queued == 0) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    return err == ESP_ERR_NOT_FINISHED ? ESP_OK : err;
}

static const uint8_t *strip_report_id_if_present(uint16_t report_id, const uint8_t *data,
        size_t *length, size_t min_payload_length)
{
    /*
     * Some stacks deliver input buffers with report ID already removed.
     * Only strip the leading byte if payload still remains above the minimum
     * expected report size. This avoids false stripping when the first data
     * byte (e.g. button bitmask) happens to equal report_id.
     */
    if (report_id != 0 && data != NULL && length != NULL &&
            *length > min_payload_length && data[0] == (uint8_t)report_id) {
        --(*length);
        return data + 1;
    }
    return data;
}

static bool wait_hid_ready(uint32_t timeout_ms)
{
    TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();

    while (!tud_hid_ready()) {
        if ((xTaskGetTickCount() - start) >= wait_ticks) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(HID_READY_POLL_MS));
    }
    return true;
}

static esp_err_t wait_and_send_keyboard(uint8_t modifier, uint8_t keycodes[6])
{
    for (int attempt = 0; attempt < HID_SEND_RETRY_COUNT; ++attempt) {
        if (wait_hid_ready(HID_READY_POLL_MS + 1) &&
            tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, keycodes)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(HID_SEND_RETRY_DELAY_MS));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_and_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel, int8_t pan)
{
    for (int attempt = 0; attempt < HID_SEND_RETRY_COUNT; ++attempt) {
        if (wait_hid_ready(HID_READY_POLL_MS + 1) &&
            tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, wheel, pan)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(HID_SEND_RETRY_DELAY_MS));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_and_send_report(uint8_t report_id, const void *report, uint16_t len)
{
    for (int attempt = 0; attempt < HID_SEND_RETRY_COUNT; ++attempt) {
        if (wait_hid_ready(HID_READY_POLL_MS + 1) &&
            tud_hid_report(report_id, report, len)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(HID_SEND_RETRY_DELAY_MS));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t enqueue_hid_tx(const hid_tx_item_t *item)
{
    if (s_hid_tx_queue == NULL || item == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_hid_tx_queue, item, pdMS_TO_TICKS(20)) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

static int8_t clamp_i16_to_i8(int16_t value)
{
    if (value > 127) {
        return 127;
    }
    if (value < -127) {
        return -127;
    }
    return (int8_t)value;
}

static int16_t sign_extend_12bit(uint16_t raw12)
{
    raw12 &= 0x0FFF;
    if ((raw12 & 0x0800U) != 0) {
        raw12 |= 0xF000U;
    }
    return (int16_t)raw12;
}

static bool looks_like_16bit_xy_report(const uint8_t *data, size_t length)
{
    bool x_hi_sign_extended;
    bool y_hi_sign_extended;

    if (data == NULL || length < 6) {
        return false;
    }

    x_hi_sign_extended = (data[2] == 0x00 || data[2] == 0xFF);
    y_hi_sign_extended = (data[4] == 0x00 || data[4] == 0xFF);
    return x_hi_sign_extended && y_hi_sign_extended;
}

typedef struct {
    uint8_t buttons;
    int8_t rel_x;
    int8_t rel_y;
    int8_t wheel;
    int8_t pan;
} mouse_packet_t;

static esp_err_t decode_mouse_packet(const uint8_t *data, size_t length, mouse_packet_t *packet)
{
    bool packed_12bit_layout = false;

    if (data == NULL || packet == NULL || length < 3) {
        return ESP_ERR_INVALID_SIZE;
    }

    packet->buttons = data[0] & 0x1F;
    packet->wheel = 0;
    packet->pan = 0;

    /*
     * Common BLE mouse report variant:
     * [reserved, buttons, X(12), Y(12), wheel, pan]
     */
    if (length >= 7 && data[1] <= 0x1F) {
        uint16_t raw_x = (uint16_t)data[2] | ((uint16_t)(data[3] & 0x0F) << 8);
        uint16_t raw_y = ((uint16_t)data[3] >> 4) | ((uint16_t)data[4] << 4);
        int16_t x12 = sign_extend_12bit(raw_x);
        int16_t y12 = sign_extend_12bit(raw_y);
        int16_t abs_x = (x12 < 0) ? -x12 : x12;
        int16_t abs_y = (y12 < 0) ? -y12 : y12;

        /* Reject obviously wrong packed decode and fall back to other layouts. */
        if (abs_x <= 1024 && abs_y <= 1024) {
            packed_12bit_layout = true;
            packet->buttons = data[1] & 0x1F;
            if (packet->buttons == 0 && (data[0] & 0x1F) != 0) {
                packet->buttons = data[0] & 0x1F;
            }
            packet->rel_x = clamp_i16_to_i8(x12 / 2);
            packet->rel_y = clamp_i16_to_i8(y12 / 2);
            packet->wheel = (int8_t)data[5];
            packet->pan = (int8_t)data[6];
        }
    }

    if (packed_12bit_layout) {
        return ESP_OK;
    }

    if (looks_like_16bit_xy_report(data, length)) {
        int16_t x16 = (int16_t)(((uint16_t)data[2] << 8) | data[1]);
        int16_t y16 = (int16_t)(((uint16_t)data[4] << 8) | data[3]);
        packet->rel_x = clamp_i16_to_i8(x16 / 2);
        packet->rel_y = clamp_i16_to_i8(y16 / 2);
        packet->wheel = (length > 5) ? (int8_t)data[5] : 0;
        packet->pan = (length > 6) ? (int8_t)data[6] : 0;
    } else {
        packet->rel_x = (int8_t)data[1];
        packet->rel_y = (int8_t)data[2];
        packet->wheel = (length > 3) ? (int8_t)data[3] : 0;
        packet->pan = (length > 4) ? (int8_t)data[4] : 0;
    }

    return ESP_OK;
}

static esp_err_t forward_keyboard_report(uint16_t report_id, const uint8_t *data, size_t length)
{
    hid_tx_item_t item = {
        .kind = HID_TX_KEYBOARD,
        .report_id = REPORT_ID_KEYBOARD,
    };
    data = strip_report_id_if_present(report_id, data, &length, 7);
    ESP_RETURN_ON_FALSE(data != NULL && length >= 7, ESP_ERR_INVALID_SIZE, TAG, "Keyboard report too short");

    if (length >= 8) {
        /* Standard keyboard report: modifier + reserved + 6 keycodes */
        item.modifier = data[0];
        memcpy(item.keycodes, data + 2, sizeof(item.keycodes));
    } else {
        /* Compact keyboard report: modifier + 6 keycodes */
        item.modifier = data[0];
        memcpy(item.keycodes, data + 1, sizeof(item.keycodes));
    }
    return enqueue_hid_tx(&item);
}

static esp_err_t forward_mouse_report(uint16_t report_id, const uint8_t *data, size_t length)
{
    mouse_packet_t primary = { 0 };
    mouse_packet_t shifted = { 0 };
    const uint8_t *primary_data = data;
    size_t primary_len = length;
    bool shifted_valid = false;
    bool use_shifted = false;

    primary_data = strip_report_id_if_present(report_id, primary_data, &primary_len,
                   primary_len >= 7 ? 7 : 3);
    ESP_RETURN_ON_FALSE(primary_data != NULL && primary_len >= 3, ESP_ERR_INVALID_SIZE, TAG, "Mouse report too short");
    ESP_RETURN_ON_ERROR(decode_mouse_packet(primary_data, primary_len, &primary), TAG, "Mouse decode failed");

    /*
     * Some devices still include report ID in the first data byte even when
     * callback metadata already has report_id. Evaluate shifted payload and
     * pick it when button decoding is clearly better.
     */
    if (report_id != 0 && data != NULL && length >= 4) {
        if (decode_mouse_packet(data + 1, length - 1, &shifted) == ESP_OK) {
            shifted_valid = true;
            if ((primary.buttons == 0 && shifted.buttons != 0) ||
                ((primary.buttons & 0x03) == 0 && (shifted.buttons & 0x03) != 0)) {
                use_shifted = true;
            }
        }
    }

    if (use_shifted && shifted_valid) {
        hid_tx_item_t item = {
            .kind = HID_TX_MOUSE,
            .report_id = REPORT_ID_MOUSE,
            .buttons = shifted.buttons,
            .x = shifted.rel_x,
            .y = shifted.rel_y,
            .wheel = shifted.wheel,
            .pan = shifted.pan,
        };
        return enqueue_hid_tx(&item);
    } else {
        hid_tx_item_t item = {
            .kind = HID_TX_MOUSE,
            .report_id = REPORT_ID_MOUSE,
            .buttons = primary.buttons,
            .x = primary.rel_x,
            .y = primary.rel_y,
            .wheel = primary.wheel,
            .pan = primary.pan,
        };
        return enqueue_hid_tx(&item);
    }
}

static esp_err_t forward_consumer_report(uint16_t report_id, const uint8_t *data, size_t length)
{
    uint16_t usage = 0;
    data = strip_report_id_if_present(report_id, data, &length, 2);
    ESP_RETURN_ON_FALSE(data != NULL && length >= 2, ESP_ERR_INVALID_SIZE, TAG, "Consumer report too short");

    usage = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    hid_tx_item_t item = {
        .kind = HID_TX_REPORT,
        .report_id = REPORT_ID_CONSUMER,
        .report_len = sizeof(usage),
    };
    memcpy(item.report, &usage, sizeof(usage));
    return enqueue_hid_tx(&item);
}

static esp_err_t forward_gamepad_report(uint16_t report_id, const uint8_t *data, size_t length)
{
    hid_gamepad_report_t report = {0};

    data = strip_report_id_if_present(report_id, data, &length, 2);
    ESP_RETURN_ON_FALSE(data != NULL && length >= 2, ESP_ERR_INVALID_SIZE, TAG, "Gamepad report too short");

    if (length >= sizeof(hid_gamepad_report_t)) {
        memcpy(&report, data, sizeof(hid_gamepad_report_t));
    } else {
        /*
         * Best-effort mapping for common compact BLE reports:
         * [buttons_lo, buttons_hi, hat, x, y, z, rz, rx, ry...]
         */
        report.buttons = (uint32_t)data[0] | ((uint32_t)data[1] << 8);
        report.hat = (length > 2) ? (data[2] & 0x0F) : GAMEPAD_HAT_CENTERED;
        report.x = (length > 3) ? (int8_t)data[3] : 0;
        report.y = (length > 4) ? (int8_t)data[4] : 0;
        report.z = (length > 5) ? (int8_t)data[5] : 0;
        report.rz = (length > 6) ? (int8_t)data[6] : 0;
        report.rx = (length > 7) ? (int8_t)data[7] : 0;
        report.ry = (length > 8) ? (int8_t)data[8] : 0;
    }

    hid_tx_item_t item = {
        .kind = HID_TX_REPORT,
        .report_id = REPORT_ID_GAMEPAD,
        .report_len = sizeof(report),
    };
    ESP_RETURN_ON_FALSE(sizeof(report) <= sizeof(item.report), ESP_ERR_INVALID_SIZE, TAG, "Gamepad report too large");
    memcpy(item.report, &report, sizeof(report));
    return enqueue_hid_tx(&item);
}

esp_err_t usb_hid_bridge_forward_input(int usage, uint16_t report_id,
                                       const uint8_t *data, size_t length)
{
    switch (usage) {
    case ESP_HID_USAGE_KEYBOARD:
        return forward_keyboard_report(report_id, data, length);
    case ESP_HID_USAGE_MOUSE:
        return forward_mouse_report(report_id, data, length);
    case ESP_HID_USAGE_GAMEPAD:
    case ESP_HID_USAGE_JOYSTICK:
        return forward_gamepad_report(report_id, data, length);
    case ESP_HID_USAGE_CCONTROL:
        return forward_consumer_report(report_id, data, length);
    default:
        ESP_LOGD(TAG, "Unsupported HID usage for USB forwarding: %d", usage);
        return ESP_ERR_NOT_SUPPORTED;
    }
}
