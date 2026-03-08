#include "vital_cluster.h"
#include "esp_log.h"
#include "esp_matter_cluster.h"
#include "esp_matter_attribute.h"
#include "esp_matter_endpoint.h"

#define TAG "vital_cl"

/* Endpoint-IDs werden beim Anlegen gespeichert */
static uint16_t s_ep_bpm_id = 0;
static uint16_t s_ep_rpm_id = 0;

/* ── Hilfsfunktion: einen Vital-Cluster mit 3 Attributen anlegen ─────────── */
static esp_err_t _create_vital_cluster(esp_matter::endpoint_t *ep,
                                        uint32_t cluster_id,
                                        uint16_t init_value,
                                        uint8_t  init_cat)
{
    /* Cluster anlegen (server-seitig, kein client flag) */
    esp_matter::cluster_t *cl =
        esp_matter::cluster::create(ep, cluster_id, esp_matter::CLUSTER_FLAG_SERVER);
    if (!cl) {
        ESP_LOGE(TAG, "Cluster 0x%08lx anlegen fehlgeschlagen", cluster_id);
        return ESP_FAIL;
    }

    /* Attribut 0x0000: value (uint16, R, Reportable) */
    esp_matter_attr_val_t val_value = esp_matter_uint16(init_value);
    esp_matter::attribute::create(cl, VITAL_ATTR_VALUE,
        esp_matter::ATTRIBUTE_FLAG_NONE | esp_matter::ATTRIBUTE_FLAG_EXTERNAL_STORAGE,
        val_value);

    /* Attribut 0x0001: category (uint8, R, Reportable) */
    esp_matter_attr_val_t val_cat = esp_matter_uint8(init_cat);
    esp_matter::attribute::create(cl, VITAL_ATTR_CATEGORY,
        esp_matter::ATTRIBUTE_FLAG_NONE | esp_matter::ATTRIBUTE_FLAG_EXTERNAL_STORAGE,
        val_cat);

    /* Attribut 0x0002: radar_status (uint8, R) – nur im BPM-Cluster sinnvoll */
    esp_matter_attr_val_t val_status = esp_matter_uint8(0);
    esp_matter::attribute::create(cl, VITAL_ATTR_RADAR_STATUS,
        esp_matter::ATTRIBUTE_FLAG_NONE | esp_matter::ATTRIBUTE_FLAG_EXTERNAL_STORAGE,
        val_status);

    ESP_LOGI(TAG, "Vital-Cluster 0x%08lx ep=%d angelegt",
             cluster_id, esp_matter::endpoint::get_id(ep));
    return ESP_OK;
}

/* ── Öffentliche API ──────────────────────────────────────────────────────── */

esp_err_t vital_clusters_create(esp_matter::endpoint_t *ep_bpm,
                                 esp_matter::endpoint_t *ep_rpm)
{
    s_ep_bpm_id = esp_matter::endpoint::get_id(ep_bpm);
    s_ep_rpm_id = esp_matter::endpoint::get_id(ep_rpm);

    esp_err_t err;
    err = _create_vital_cluster(ep_bpm, VITAL_CLUSTER_HEARTRATE,  0, 0);
    if (err != ESP_OK) return err;
    err = _create_vital_cluster(ep_rpm, VITAL_CLUSTER_BREATHRATE, 0, 0);
    return err;
}

esp_err_t vital_clusters_update(uint16_t bpm, uint8_t bpm_cat,
                                 uint16_t rpm, uint8_t rpm_cat,
                                 uint8_t radar_status)
{
    esp_err_t err = ESP_OK;

    /* Herzrate – value */
    esp_matter_attr_val_t v = esp_matter_uint16(bpm);
    err |= esp_matter::attribute::update(s_ep_bpm_id,
               VITAL_CLUSTER_HEARTRATE, VITAL_ATTR_VALUE, &v);

    /* Herzrate – category */
    v = esp_matter_uint8(bpm_cat);
    err |= esp_matter::attribute::update(s_ep_bpm_id,
               VITAL_CLUSTER_HEARTRATE, VITAL_ATTR_CATEGORY, &v);

    /* Herzrate – radar_status */
    v = esp_matter_uint8(radar_status);
    err |= esp_matter::attribute::update(s_ep_bpm_id,
               VITAL_CLUSTER_HEARTRATE, VITAL_ATTR_RADAR_STATUS, &v);

    /* Atemrate – value */
    v = esp_matter_uint16(rpm);
    err |= esp_matter::attribute::update(s_ep_rpm_id,
               VITAL_CLUSTER_BREATHRATE, VITAL_ATTR_VALUE, &v);

    /* Atemrate – category */
    v = esp_matter_uint8(rpm_cat);
    err |= esp_matter::attribute::update(s_ep_rpm_id,
               VITAL_CLUSTER_BREATHRATE, VITAL_ATTR_CATEGORY, &v);

    if (err != ESP_OK)
        ESP_LOGW(TAG, "attribute::update Fehler: %d", err);

    return err;
}
