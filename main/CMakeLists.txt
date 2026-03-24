idf_component_register(
    SRCS
        "main.c"
        "ha_mqtt.c"
        "zb_gateway.c"
        "zb_ota_server.c"
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
)
