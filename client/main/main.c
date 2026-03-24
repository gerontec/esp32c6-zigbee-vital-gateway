#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ieee802154.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_zigbee_core.h"
#include "ha_mqtt.h"
#include "zb_device.h"

#define TAG              "main"
#define BTN_GPIO         9       /* Boot-Taste GPIO9 */
#define HEARTBEAT_MS     60000

/* ── Kommando-Handler: Befehle vom Pi ───────────────────────────────────── */
static void on_cmd(const char *cmd, const char *payload, int len) {
    (void)len;
    if (strcmp(cmd, "leave") == 0) {
        zb_device_leave();
    } else if (strcmp(cmd, "set_onoff") == 0) {
        zb_device_set_onoff(atoi(payload) != 0);
    } else if (strcmp(cmd, "permit_join") == 0) {
        zb_device_permit_join((uint8_t)atoi(payload));
    } else if (strcmp(cmd, "set_channel") == 0) {
        zb_device_set_channel((uint8_t)atoi(payload));
    }
}

/* ── Heartbeat-Task ─────────────────────────────────────────────────────── */
static void heartbeat_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
        uint32_t up   = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        int8_t   rssi = esp_ieee802154_get_recent_rssi();
        ha_mqtt_publish_heartbeat(up, zb_device_pan_id(), zb_device_channel(), rssi);
    }
}

/* ── Permit-Join-Taste (Boot-Taste) ─────────────────────────────────────── */
static void btn_task(void *arg) {
    gpio_set_direction(BTN_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_GPIO, GPIO_PULLUP_ONLY);
    bool last = true;
    while (1) {
        bool cur = gpio_get_level(BTN_GPIO);
        if (last && !cur) {
            if (zb_device_joined()) {
                /* Kurzer Druck: Permit Join 60 s */
                zb_device_permit_join(60);
            } else {
                /* Netz suchen */
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
        }
        last = cur;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ── app_main ───────────────────────────────────────────────────────────── */
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ha_mqtt_set_cmd_cb(on_cmd);
    ESP_ERROR_CHECK(ha_mqtt_init());

    ha_mqtt_publish_boot();

    zb_device_start();

    xTaskCreate(btn_task,       "btn",       2048, NULL, 3, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 2, NULL);
}
