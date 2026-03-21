idf_component_register(
    SRCS
        "main.c"
        "ha_mqtt.c"
        "zb_device.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        driver
        esp_timer
        espressif__esp-zigbee-lib
        freertos
        log
)
