#include "matter_endpoint.h"
#include "vital_cluster.h"
#include "esp_log.h"
#include "esp_matter.h"
#include "esp_matter_cluster.h"
#include "esp_matter_endpoint.h"

#define TAG "matter_ep"

static uint16_t s_ep_occupancy = 0;

esp_err_t matter_endpoints_init(void)
{
    esp_matter::node::config_t node_cfg = {};
    esp_matter::node_t *node = esp_matter::node::create(&node_cfg, NULL, NULL);
    if (!node) return ESP_FAIL;

    /* ── Endpoint 1: Occupancy Sensing (Radar-Präsenz) ───────────── */
    esp_matter::endpoint::occupancy_sensor::config_t occ_cfg = {};
    esp_matter::endpoint_t *ep_occ =
        esp_matter::endpoint::occupancy_sensor::create(
            node, &occ_cfg, ENDPOINT_FLAG_NONE, NULL);
    s_ep_occupancy = esp_matter::endpoint::get_id(ep_occ);
    ESP_LOGI(TAG, "Occupancy endpoint id=%d", s_ep_occupancy);

    /* ── Endpoint 2: Custom Herzrate (VITAL_CLUSTER_HEARTRATE) ─────── */
    esp_matter::endpoint::config_t bpm_cfg = {};
    esp_matter::endpoint_t *ep_bpm =
        esp_matter::endpoint::create(node, &bpm_cfg, ENDPOINT_FLAG_NONE, NULL);
    ESP_LOGI(TAG, "BPM endpoint id=%d", esp_matter::endpoint::get_id(ep_bpm));

    /* ── Endpoint 3: Custom Atemrate (VITAL_CLUSTER_BREATHRATE) ────── */
    esp_matter::endpoint::config_t rpm_cfg = {};
    esp_matter::endpoint_t *ep_rpm =
        esp_matter::endpoint::create(node, &rpm_cfg, ENDPOINT_FLAG_NONE, NULL);
    ESP_LOGI(TAG, "RPM endpoint id=%d", esp_matter::endpoint::get_id(ep_rpm));

    /* Custom Vital-Cluster an ep_bpm und ep_rpm hängen */
    esp_err_t err = vital_clusters_create(ep_bpm, ep_rpm);
    if (err != ESP_OK) return err;

    return esp_matter::start(NULL);
}

esp_err_t matter_update_vitals(uint16_t bpm, uint16_t rpm,
                                uint8_t bpm_cat, uint8_t rpm_cat,
                                uint8_t radar_status)
{
    /* Occupancy: radar_status 2 = measuring → belegt */
    esp_matter_attr_val_t occ = esp_matter_uint8(radar_status == 2 ? 1 : 0);
    esp_matter::attribute::update(
        s_ep_occupancy,
        esp_matter::cluster::occupancy_sensing::Id,
        esp_matter::cluster::occupancy_sensing::attribute::occupancy::Id,
        &occ);

    /* BPM + RPM in Custom-Cluster */
    return vital_clusters_update(bpm, bpm_cat, rpm, rpm_cat, radar_status);
}
