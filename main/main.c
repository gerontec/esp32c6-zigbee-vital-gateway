#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mr60bha1.h"
#include "ha_mqtt.h"
#include "zb_gateway.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  KONFIGURATION – hier anpassen
 * ══════════════════════════════════════════════════════════════════════════ */
#define WIFI_SSID        "MeinNetz"
#define WIFI_PASSWORD    "MeinPasswort"
#define MQTT_BROKER_URI  "mqtt://192.168.1.10"
#define MQTT_USER        NULL
#define MQTT_PASS        NULL

/* MR60BHA1 UART-Pins (ESP32-C6 DevKit) */
#define MR60_UART        UART_NUM_1
#define MR60_TX_PIN      4
#define MR60_RX_PIN      5

/* Permit-Join-Taste (Boot-Taste = GPIO9) */
#define BTN_PERMIT_JOIN  9
#define PERMIT_JOIN_SECS 180
/* ══════════════════════════════════════════════════════════════════════════ */

#define TAG "main"
#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_events;

/* ── WiFi ───────────────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi getrennt – verbinde erneut");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void) {
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                wifi_event_handler, NULL);

    wifi_config_t wcfg = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASSWORD }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Verbinde mit %s …", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

/* ── MR60BHA1-Callback → MQTT ───────────────────────────────────────────── */
static void on_radar_frame(const mr60_data_t *d) {
    ha_mqtt_publish_vitals(d);
}

/* ── MQTT-Kommando-Handler (Befehle vom Linux-Host) ─────────────────────── */
static void on_mqtt_cmd(const char *cmd, const char *payload, int len) {
    if (strcmp(cmd, "permit_join") == 0) {
        /* Payload: Anzahl Sekunden als ASCII-Dezimalzahl, z.B. "180" oder "0" */
        char buf[8] = {0};
        int n = len < (int)(sizeof(buf) - 1) ? len : (int)(sizeof(buf) - 1);
        memcpy(buf, payload, n);
        uint8_t secs = (uint8_t)atoi(buf);
        ESP_LOGI(TAG, "cmd permit_join %u s", secs);
        zb_gateway_permit_join(secs);
    } else {
        ESP_LOGW(TAG, "Unbekanntes Kommando: %s", cmd);
    }
}

/* ── Permit-Join-Taste (Notfall ohne Linux-Host) ─────────────────────────── */
static void btn_task(void *arg) {
    gpio_set_direction(BTN_PERMIT_JOIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_PERMIT_JOIN, GPIO_PULLUP_ONLY);

    bool last = true;
    while (1) {
        bool cur = gpio_get_level(BTN_PERMIT_JOIN);
        if (last && !cur) {  /* fallende Flanke */
            ESP_LOGI(TAG, "Permit Join %d s (Taste)", PERMIT_JOIN_SECS);
            zb_gateway_permit_join(PERMIT_JOIN_SECS);
        }
        last = cur;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ── app_main ───────────────────────────────────────────────────────────── */
void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-C6 Radio Bridge – minimal ===");

    /* NVS (wird von WiFi und Zigbee benötigt) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 1. WiFi */
    wifi_init();

    /* 2. MQTT-Bridge (kein HA-spezifisches Wissen) */
    ha_mqtt_set_cmd_cb(on_mqtt_cmd);
    ESP_ERROR_CHECK(ha_mqtt_init(MQTT_BROKER_URI, MQTT_USER, MQTT_PASS));

    /* 3. MR60BHA1 Radar */
    ESP_ERROR_CHECK(mr60_init(MR60_UART, MR60_TX_PIN, MR60_RX_PIN,
                              on_radar_frame));

    /* 4. Zigbee-Koordinator (startet eigenen Task) */
    zb_gateway_start();

    /* 5. Permit-Join-Taste als Fallback */
    xTaskCreate(btn_task, "btn", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG,
        "Gestartet.\n"
        "  MQTT-Broker  : %s\n"
        "  Base-Topic   : %s\n"
        "  Zigbee       : Koordinator aktiv\n"
        "  Permit Join  : Boot-Taste (GPIO%d) ODER MQTT cmd/permit_join",
        MQTT_BROKER_URI,
        ha_mqtt_base_topic(),
        BTN_PERMIT_JOIN);
}
