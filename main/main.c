#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mr60bha2.h"
#include "ha_mqtt.h"
#include "zb_gateway.h"

/* MR60BHA2 UART-Pins (ESP32-C6 DevKit) */
#define MR60_UART        UART_NUM_1
#define MR60_TX_PIN      4
#define MR60_RX_PIN      5

/* Permit-Join-Taste (Boot-Taste = GPIO9) */
#define BTN_PERMIT_JOIN  9
#define PERMIT_JOIN_SECS 180

#define TAG "main"

/* ── MR60BHA2-Callback → UART ───────────────────────────────────────────── */
static void on_radar_frame(const mr60_data_t *d) {
    ha_mqtt_publish_vitals(d);
}

/* ── UART-Kommando-Handler (Befehle vom S3) ─────────────────────────────── */
static void on_uart_cmd(const char *cmd, const char *payload, int len) {
    if (strcmp(cmd, "permit_join") == 0) {
        char buf[8] = {0};
        int n = len < (int)(sizeof(buf) - 1) ? len : (int)(sizeof(buf) - 1);
        memcpy(buf, payload, n);
        uint8_t secs = (uint8_t)atoi(buf);
        ESP_LOGI(TAG, "permit_join %u s (via UART vom S3)", secs);
        zb_gateway_permit_join(secs);
    }
}

/* ── Permit-Join-Taste (Notfall ohne S3) ────────────────────────────────── */
static void btn_task(void *arg) {
    gpio_set_direction(BTN_PERMIT_JOIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_PERMIT_JOIN, GPIO_PULLUP_ONLY);

    bool last = true;
    while (1) {
        bool cur = gpio_get_level(BTN_PERMIT_JOIN);
        if (last && !cur) {
            ESP_LOGI(TAG, "Permit Join %d s (Boot-Taste)", PERMIT_JOIN_SECS);
            zb_gateway_permit_join(PERMIT_JOIN_SECS);
        }
        last = cur;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ── app_main ───────────────────────────────────────────────────────────── */
void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-C6  Zigbee + Radar → UART Bridge ===");

    /* NVS (wird von Zigbee benötigt) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 1. UART-Serial statt MQTT */
    ha_mqtt_set_cmd_cb(on_uart_cmd);
    ESP_ERROR_CHECK(ha_mqtt_init(NULL, NULL, NULL));

    /* 2. MR60BHA2 Radar (60 GHz, UART1, TX=GPIO4 RX=GPIO5) */
    ESP_ERROR_CHECK(mr60bha2_init(MR60_UART, MR60_TX_PIN, MR60_RX_PIN,
                                  on_radar_frame));

    /* 3. Zigbee-Koordinator */
    zb_gateway_start();

    /* 4. Permit-Join-Taste als Fallback */
    xTaskCreate(btn_task, "btn", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG,
        "Gestartet.\n"
        "  JSON-Output : UART0 TX=GPIO16\n"
        "  Radar       : UART1 TX=GPIO4 RX=GPIO5\n"
        "  Zigbee      : Koordinator aktiv\n"
        "  Permit Join : Boot-Taste (GPIO%d) ODER cmd vom S3",
        BTN_PERMIT_JOIN);
}
