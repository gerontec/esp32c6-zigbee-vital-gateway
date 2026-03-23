/*
 * zb_ota_server.c – Zigbee OTA Upgrade Server
 *
 * Die Pi-seitige Binary wird on-demand über UART angefragt:
 *   1. next_data_cb sendet {"t":"ota_req","off":O,"sz":S} an Pi
 *   2. Pi antwortet mit {"cmd":"ota_data","off":O,"data":"HEX"}
 *   3. zb_ota_server_feed_data() dekodiert Hex → gibt Semaphor frei
 *   4. next_data_cb kehrt mit Daten zurück
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_ota.h"
#include "esp_zigbee_attribute.h"
#include "zcl/esp_zigbee_zcl_ota.h"
#include "ha_mqtt.h"
#include "zb_ota_server.h"

#define TAG            "ota_srv"
#define OTA_ENDPOINT   1
#define OTA_MFR_CODE   0x131B
#define OTA_IMAGE_TYPE 0x0001
#define OTA_MAX_DATA   64
#define OTA_TIMEOUT_MS 10000

static SemaphoreHandle_t s_data_sem  = NULL;
static uint8_t  s_buf[OTA_MAX_DATA];
static bool     s_ota_active = false;

/* ── Callback: Stack fordert nächsten Chunk an ──────────────────────────── */
static esp_err_t ota_next_data_cb(esp_zb_ota_zcl_information_t msg,
                                   uint16_t index, uint8_t size, uint8_t **data)
{
    (void)msg;
    if (!s_ota_active) return ESP_ERR_INVALID_STATE;

    char req[64];
    snprintf(req, sizeof(req),
             "{\"t\":\"ota_req\",\"off\":%u,\"sz\":%u}",
             (unsigned)index, (unsigned)size);
    ha_mqtt_emit_raw(req);

    if (xSemaphoreTake(s_data_sem, pdMS_TO_TICKS(OTA_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout ota_data off=%u", (unsigned)index);
        return ESP_ERR_TIMEOUT;
    }
    *data = s_buf;
    return ESP_OK;
}

/* ── Chunk vom Pi empfangen (rx_task → ha_mqtt cmd_cb) ──────────────────── */
void zb_ota_server_feed_data(const char *hex, uint8_t byte_count)
{
    if (!s_data_sem || !s_ota_active) return;
    size_t n = byte_count < OTA_MAX_DATA ? byte_count : OTA_MAX_DATA;
    for (size_t i = 0; i < n; i++) {
        char h[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        s_buf[i] = (uint8_t)strtol(h, NULL, 16);
    }
    xSemaphoreGive(s_data_sem);
}

/* ── OTA-Session starten ─────────────────────────────────────────────────── */
void zb_ota_server_start(uint32_t size, uint32_t version)
{
    if (!s_data_sem) {
        s_data_sem = xSemaphoreCreateBinary();
    }
    s_ota_active = true;

    esp_zb_ota_upgrade_server_notify_req_t req = {
        .endpoint         = OTA_ENDPOINT,
        .index            = 0,
        .notify_on        = 1,
        .ota_upgrade_time = 0,
        .ota_file_header  = {
            .manufacturer_code = OTA_MFR_CODE,
            .image_type        = OTA_IMAGE_TYPE,
            .file_version      = version,
            .image_size        = size,
        },
        .next_data_cb = ota_next_data_cb,
    };
    esp_err_t err = esp_zb_ota_upgrade_server_notify_req(&req);

    char line[96];
    snprintf(line, sizeof(line),
             "{\"t\":\"ota_status\",\"status\":\"%s\",\"size\":%lu,\"ver\":%lu}",
             err == ESP_OK ? "started" : "error",
             (unsigned long)size, (unsigned long)version);
    ha_mqtt_emit_raw(line);
    ESP_LOGI(TAG, "OTA server %s size=%lu ver=%lu",
             err == ESP_OK ? "started" : "error",
             (unsigned long)size, (unsigned long)version);
}

/* ── OTA-Server-Cluster-Attribute erzeugen ───────────────────────────────── */
esp_zb_attribute_list_t *zb_ota_server_cluster_create(void)
{
    esp_zb_attribute_list_t *attr = esp_zb_zcl_attr_list_create(
        ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE);

    esp_zb_zcl_ota_upgrade_server_variable_t var = {
        .query_jitter = 100,
        .current_time = 0,
        .file_count   = 1,
    };
    esp_zb_ota_cluster_add_attr(attr,
        ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_DATA_ID, &var);
    return attr;
}
