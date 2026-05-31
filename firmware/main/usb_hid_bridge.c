#include "usb_hid_bridge.h"

#include <stdbool.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "class/cdc/cdc_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_hid";

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE 2

#define ITF_NUM_CDC 0
#define ITF_NUM_CDC_DATA 1
#define ITF_NUM_HID 2
#define ITF_NUM_TOTAL 3

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define EPNUM_HID_IN 0x83

#define USB_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

static QueueHandle_t s_cdc_rx_queue;
static uint8_t s_cdc_rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
static bool s_driver_ready;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
};

static const char *s_string_descriptor[] = {
    (const char[]){ 0x09, 0x04 },
    "YunaUnion",
    "ESP32-S3 BLE HID Dongle",
    "000001",
    "Management CDC",
    "Keyboard and Mouse HID",
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

static const uint8_t *strip_report_id(uint16_t report_id, const uint8_t *data,
                                      size_t *length)
{
    if (report_id != 0 && data != NULL && length != NULL && *length > 0 && data[0] == (uint8_t)report_id) {
        --(*length);
        return data + 1;
    }
    return data;
}

static esp_err_t forward_keyboard_report(uint16_t report_id, const uint8_t *data, size_t length)
{
    data = strip_report_id(report_id, data, &length);
    ESP_RETURN_ON_FALSE(data != NULL && length >= 8, ESP_ERR_INVALID_SIZE, TAG, "Keyboard report too short");
    ESP_RETURN_ON_FALSE(tud_hid_ready(), ESP_ERR_INVALID_STATE, TAG, "USB HID is not ready");

    uint8_t keycodes[6] = { 0 };
    memcpy(keycodes, data + 2, sizeof(keycodes));
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, data[0], keycodes);
    return ESP_OK;
}

static esp_err_t forward_mouse_report(uint16_t report_id, const uint8_t *data, size_t length)
{
    data = strip_report_id(report_id, data, &length);
    ESP_RETURN_ON_FALSE(data != NULL && length >= 3, ESP_ERR_INVALID_SIZE, TAG, "Mouse report too short");
    ESP_RETURN_ON_FALSE(tud_hid_ready(), ESP_ERR_INVALID_STATE, TAG, "USB HID is not ready");

    int8_t wheel = length > 3 ? (int8_t)data[3] : 0;
    int8_t pan = length > 4 ? (int8_t)data[4] : 0;
    tud_hid_mouse_report(REPORT_ID_MOUSE, data[0], (int8_t)data[1], (int8_t)data[2], wheel, pan);
    return ESP_OK;
}

esp_err_t usb_hid_bridge_forward_input(int usage, uint16_t report_id,
                                       const uint8_t *data, size_t length)
{
    switch (usage) {
    case ESP_HID_USAGE_KEYBOARD:
        return forward_keyboard_report(report_id, data, length);
    case ESP_HID_USAGE_MOUSE:
        return forward_mouse_report(report_id, data, length);
    default:
        ESP_LOGD(TAG, "Unsupported HID usage for USB forwarding: %d", usage);
        return ESP_ERR_NOT_SUPPORTED;
    }
}
