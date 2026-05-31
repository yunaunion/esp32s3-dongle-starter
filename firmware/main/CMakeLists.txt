idf_component_register(
    SRCS
        "main.c"
        "manager_protocol.c"
        "pairing_store.c"
        "ble_hid_bridge.c"
        "usb_hid_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        nvs_flash
        json
)

