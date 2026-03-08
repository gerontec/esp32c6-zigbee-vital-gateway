#pragma once
#include <stdint.h>
#include "esp_err.h"

/**
 * Matter over Thread – Endpoint-Definitionen
 *
 * Mapping MR60BHA1 → Matter Data Model:
 *
 *   Endpoint 1: occupancy-sensing  (Radar-Präsenz → occ)
 *   Endpoint 2: air-quality        (Herzrate als custom cluster, BPM)
 *   Endpoint 3: air-quality        (Atemrate, RPM)
 *
 * Zigbee-Cluster-Äquivalente:
 *   Zigbee OCCUPANCY_SENSING (0x0406) → Matter OccupancySensing cluster
 *   Zigbee (custom vital)            → Matter custom cluster 0xFFF10001
 *
 * Thread-Netz: ESP32-C6 agiert als Matter End Device (nicht Coordinator).
 * Border Router (z.B. RPi + ot-br-posix) verbindet Thread → IP-Netz.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Matter-Endpoints initialisieren und im Data Model registrieren. */
esp_err_t matter_endpoints_init(void);

/** Vitalwerte in Matter-Attribute schreiben (thread-safe). */
esp_err_t matter_update_vitals(uint16_t bpm, uint16_t rpm,
                                uint8_t bpm_cat, uint8_t rpm_cat,
                                uint8_t radar_status);

#ifdef __cplusplus
}
#endif
