#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_matter.h"

/**
 * Custom Matter Cluster – Vital Signs (MR60BHA1)
 *
 * Manufacturer-specific cluster IDs (0xFC00–0xFFFE are vendor-specific):
 *   0xFFF10001  HeartRate  cluster
 *   0xFFF10002  BreathRate cluster
 *
 * Attribute IDs (both clusters identical layout):
 *   0x0000  value      uint16  – BPM / RPM (Messwert)
 *   0x0001  category   uint8   – 0=none 1=normal 2=fast 3=slow
 *   0x0002  radar_status uint8 – 0=init 1=calibrating 2=measuring
 *
 * Cluster-Flags: server-side, kein client nötig.
 */

#define VITAL_CLUSTER_HEARTRATE    0xFFF10001UL
#define VITAL_CLUSTER_BREATHRATE   0xFFF10002UL

#define VITAL_ATTR_VALUE           0x0000
#define VITAL_ATTR_CATEGORY        0x0001
#define VITAL_ATTR_RADAR_STATUS    0x0002

/**
 * Beide Custom-Cluster an den angegebenen Endpoints anlegen.
 * Muss nach esp_matter::node::create() aufgerufen werden.
 *
 * @param ep_bpm  Endpoint-Handle für Herzrate
 * @param ep_rpm  Endpoint-Handle für Atemrate
 */
esp_err_t vital_clusters_create(esp_matter::endpoint_t *ep_bpm,
                                 esp_matter::endpoint_t *ep_rpm);

/**
 * Messwerte in die Cluster-Attribute schreiben (thread-safe).
 * Löst automatisch Matter-Attribute-Reports aus.
 */
esp_err_t vital_clusters_update(uint16_t bpm, uint8_t bpm_cat,
                                 uint16_t rpm, uint8_t rpm_cat,
                                 uint8_t radar_status);
