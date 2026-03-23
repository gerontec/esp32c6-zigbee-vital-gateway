#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_zigbee_core.h"
#include "ha_mqtt.h"
#include "zb_gateway.h"
#include "zb_ota_server.h"

/* Permit-Join-Taste (Boot-Taste = GPIO9) */
#define BTN_PERMIT_JOIN  9
#define PERMIT_JOIN_SECS 180

#define TAG "main"

/* ── Kommando-Handler: Befehle vom Pi via UART1 ─────────────────────────── */
static void on_uart_cmd(const char *cmd, const char *payload, int len) {
    char buf[8] = {0};
    int n = len < (int)(sizeof(buf) - 1) ? len : (int)(sizeof(buf) - 1);
    memcpy(buf, payload, n);

    if (strcmp(cmd, "permit_join") == 0) {
        uint8_t secs = (uint8_t)atoi(buf);
        ESP_LOGI(TAG, "permit_join %u s", secs);
        zb_gateway_permit_join(secs);
    } else if (strcmp(cmd, "scan_chan") == 0) {
        zb_gateway_scan_channels();
    } else if (strcmp(cmd, "set_channel") == 0) {
        uint8_t ch = (uint8_t)atoi(buf);
        ESP_LOGI(TAG, "set_channel %u", ch);
        zb_gateway_set_channel(ch);
    } else if (strcmp(cmd, "ota_start") == 0) {
        /* payload: "SIZE,VERSION" */
        uint32_t size = (uint32_t)atol(payload);
        const char *comma = strchr(payload, ',');
        uint32_t ver = comma ? (uint32_t)atol(comma + 1) : 1;
        zb_ota_server_start(size, ver);
    } else if (strcmp(cmd, "ota_data") == 0) {
        /* payload = hex string, len = byte count */
        zb_ota_server_feed_data(payload, (uint8_t)len);
    }
}

/* ── Heartbeat-Task: alle 60 s Status-JSON senden ───────────────────────── */
#define HEARTBEAT_INTERVAL_MS 60000

static void heartbeat_task(void *arg) {
    char lqi_buf[256];
    char line[384];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        uint8_t  ch       = esp_zb_get_current_channel();
        uint16_t pan      = esp_zb_get_pan_id();
        zb_gateway_lqi_json(lqi_buf, sizeof(lqi_buf));
        snprintf(line, sizeof(line),
            "{\"t\":\"heartbeat\",\"uptime\":%lu,\"ch\":%u,\"pan\":\"0x%04x\",\"dev\":%s}",
            (unsigned long)uptime_s, (unsigned)ch, (unsigned)pan, lqi_buf);
        ha_mqtt_emit_raw(line);
    }
}

/* ── Permit-Join-Taste ──────────────────────────────────────────────────── */
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
    ESP_LOGI(TAG, "=== ESP32-C6 Zigbee Gateway ===");
    ESP_LOGI(TAG, "  JSON : UART1 TX=GPIO16 RX=GPIO17 → Pi /dev/ttyUSBx");
    ESP_LOGI(TAG, "  Logs : UART0 / USB-CDC → /dev/ttyACM0");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ha_mqtt_set_cmd_cb(on_uart_cmd);
    ESP_ERROR_CHECK(ha_mqtt_init(NULL, NULL, NULL));

    zb_gateway_start();

    xTaskCreate(btn_task,       "btn",       2048, NULL, 3, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 2, NULL);
}
