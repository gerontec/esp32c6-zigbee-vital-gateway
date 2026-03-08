idf_component_register(
    SRCS
        "main.c"
        "mr60bha1.c"
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
        esp_zigbee_core
        freertos
        log
)
