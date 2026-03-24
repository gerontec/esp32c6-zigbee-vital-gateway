idf_component_register(
    SRCS
        "main.c"
        "ha_mqtt.c"
        "zb_device.c"
        "zb_ota_client.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        driver
        esp_timer
        espressif__esp-zigbee-lib
        app_update
        freertos
        log
        ieee802154
        espressif__onewire_bus
)
