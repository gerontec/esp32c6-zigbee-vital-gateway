#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

/* MR60BHA2 – 60 GHz mmWave Breathing & Heartbeat Pro */
#include "mr60bha2.h"
#include "matter_endpoint.h"

#define TAG "main"

/* UART-Pins (same wiring as BHA1) */
#define MR60_UART    UART_NUM_1
#define MR60_TX_PIN  4
#define MR60_RX_PIN  5

/* WiFi Fallback falls kein offenes Netz gefunden */
#define WIFI_FALLBACK_SSID  "free5G"
#define WIFI_FALLBACK_PASS  "dach1234"

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
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                wifi_event_handler, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Scan nach offenem Netz */
    ESP_LOGI(TAG, "Scanne nach offenem WiFi …");
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_wifi_scan_start(&scan_cfg, true);   /* blockierend */

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    char open_ssid[33] = "";
    if (ap_count > 0) {
        wifi_ap_record_t *aps = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (aps) {
            esp_wifi_scan_get_ap_records(&ap_count, aps);
            for (int i = 0; i < ap_count; i++) {
                if (aps[i].authmode == WIFI_AUTH_OPEN) {
                    strlcpy(open_ssid, (char *)aps[i].ssid, sizeof(open_ssid));
                    ESP_LOGI(TAG, "Offenes Netz gefunden: %s (RSSI %d)",
                             open_ssid, aps[i].rssi);
                    break;
                }
            }
            free(aps);
        }
    }

    wifi_config_t wcfg = {0};
    if (open_ssid[0]) {
        strlcpy((char *)wcfg.sta.ssid, open_ssid, sizeof(wcfg.sta.ssid));
        ESP_LOGI(TAG, "Verbinde mit offenem Netz: %s", open_ssid);
    } else {
        strlcpy((char *)wcfg.sta.ssid,     WIFI_FALLBACK_SSID, sizeof(wcfg.sta.ssid));
        strlcpy((char *)wcfg.sta.password, WIFI_FALLBACK_PASS, sizeof(wcfg.sta.password));
        ESP_LOGI(TAG, "Kein offenes Netz – Fallback: %s", WIFI_FALLBACK_SSID);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

static void mr60_callback(const mr60_data_t *d)
{
    ESP_LOGI(TAG, "bpm=%d rpm=%d cat=%d/%d status=%d",
             d->bpm, d->rpm, d->bpm_category, d->rpm_category, d->status);
    matter_update_vitals(d->bpm, d->rpm, d->bpm_category, d->rpm_category, d->status);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Matter over WiFi – Vital Gateway");

    /* NVS (Matter benötigt NVS für Commissioning-Daten) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* WiFi: offenes Netz suchen, sonst Fallback */
    wifi_init();

    /* Matter-Endpoints registrieren und Stack starten */
    ESP_ERROR_CHECK(matter_endpoints_init());
    ESP_LOGI(TAG, "Matter Stack gestartet");

    /* MR60BHA2 Radar-Sensor (UART 115200, TX=GPIO4, RX=GPIO5) */
    ESP_ERROR_CHECK(mr60bha2_init(MR60_UART, MR60_TX_PIN, MR60_RX_PIN,
                                   mr60_callback));
    ESP_LOGI(TAG, "MR60BHA2 initialisiert");

    ESP_LOGI(TAG, "Bereit – Matter aktiv, WiFi verbunden");
    /* Matter-Stack läuft in eigenen Tasks, main kann schlafen */
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
