idf_component_register(
    SRCS
        "main.c"
        "mr60bha2.c"
        "ha_mqtt.c"
        "zb_gateway.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        mqtt
        driver
        espressif__esp-zigbee-lib
        freertos
        log
)
