/*
 * wifi_switch.c – WiFi-Modus für WIFI_SWITCH_TIMEOUT_S, dann esp_restart()
 *
 * Ablauf:
 *   1. Empfang des switch2wifi-Befehls via Zigbee → wifi_switch_trigger()
 *   2. NVS-Flag setzen + esp_restart()
 *   3. Nächster Boot: app_main prüft Flag → wifi_switch_run()
 *   4. WiFi scan → bestes offenes AP → verbinden → warten → esp_restart()
 *   5. Nächster Boot: normaler Zigbee-Betrieb
 */
#include "wifi_switch.h"
#include "ha_mqtt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include <string.h>

#define TAG      "wifi_sw"
#define NVS_NS   "wifi_sw"
#define NVS_KEY  "pending"
#define SCAN_MAX 20

/* ── NVS-Flag ────────────────────────────────────────────────────────────── */
bool wifi_switch_is_pending(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &v);
        nvs_close(h);
    }
    return v == 1;
}

static void set_pending(uint8_t val) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Aus Zigbee-Callback aufrufen: Flag setzen + sofortiger Neustart */
void wifi_switch_trigger(void) {
    set_pending(1);
    ha_mqtt_emit_raw("{\"t\":\"switch2wifi\",\"state\":\"rebooting\"}");
    /* Kurze Verzögerung damit emit() flushen kann, dann Neustart */
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

/* ── WiFi-Modus (läuft in app_main vor Zigbee-Start) ───────────────────── */
void wifi_switch_run(void) {
    set_pending(0);   /* Flag sofort löschen */

    ha_mqtt_emit_raw("{\"t\":\"switch2wifi\",\"state\":\"starting\"}");

    /* Event-Loop + Netif init (einmalig, da kein Zigbee gestartet wird) */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Scan starten (blocking) */
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false,
    };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t         ap_count = SCAN_MAX;
    wifi_ap_record_t recs[SCAN_MAX];
    memset(recs, 0, sizeof(recs));
    esp_wifi_scan_get_ap_records(&ap_count, recs);

    /* Bestes offenes AP (stärkstes Signal, kein Passwort) */
    int best = -1, best_rssi = -128;
    for (int i = 0; i < ap_count; i++) {
        if (recs[i].authmode == WIFI_AUTH_OPEN && recs[i].rssi > best_rssi) {
            best_rssi = recs[i].rssi;
            best      = i;
        }
    }

    if (best < 0) {
        char line[80];
        snprintf(line, sizeof(line),
            "{\"t\":\"switch2wifi\",\"state\":\"no_open_ap\",\"scanned\":%d}",
            ap_count);
        ha_mqtt_emit_raw(line);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    char ssid[33] = {0};
    memcpy(ssid, recs[best].ssid, 32);

    char line[160];
    snprintf(line, sizeof(line),
        "{\"t\":\"switch2wifi\",\"state\":\"connecting\",\"ssid\":\"%s\",\"rssi\":%d}",
        ssid, best_rssi);
    ha_mqtt_emit_raw(line);

    /* Verbinden */
    wifi_config_t wifi_cfg = {};
    memcpy(wifi_cfg.sta.ssid, recs[best].ssid, 32);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    esp_wifi_connect();

    /* Verbindungsaufbau abwarten */
    vTaskDelay(pdMS_TO_TICKS(6000));

    snprintf(line, sizeof(line),
        "{\"t\":\"switch2wifi\",\"state\":\"connected\",\"ssid\":\"%s\",\"timeout_s\":%d}",
        ssid, WIFI_SWITCH_TIMEOUT_S);
    ha_mqtt_emit_raw(line);

    /* Timeout laufen lassen */
    vTaskDelay(pdMS_TO_TICKS((uint32_t)WIFI_SWITCH_TIMEOUT_S * 1000));

    ha_mqtt_emit_raw("{\"t\":\"switch2wifi\",\"state\":\"timeout_reboot\"}");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
