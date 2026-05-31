idf_component_register(
    SRCS
        "main.c"
        "manager_protocol.c"
        "pairing_store.c"
        "ble_hid_bridge.c"
        "usb_hid_bridge.c"
        "status_led.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        esp_hid
        nvs_flash
        json
        esp_tinyusb
        esp_driver_gpio
        esp_driver_rmt
)
