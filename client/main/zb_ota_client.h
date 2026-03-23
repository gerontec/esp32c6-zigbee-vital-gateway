#pragma once
#include "esp_err.h"
#include "esp_zigbee_core.h"

/*
 * zb_ota_client – Zigbee OTA Upgrade Client (Router-Seite)
 *
 * Empfängt Firmware-Updates vom Coordinator via Zigbee OTA Cluster.
 * Schreibt Daten in die zweite OTA-Partition und bootet nach Fertigstellung.
 *
 * C6→Pi (via ha_mqtt_publish_ota_status):
 *   {"t":"ota_progress","status":"started"|"receiving"|"applying"|"done"|"error",
 *    "off":<bytes_written>,"total":<total_bytes>}
 */

/* Erstellt die OTA-Client-Cluster-Attribute für den Endpoint-Aufbau */
esp_zb_attribute_list_t *zb_ota_client_cluster_create(void);

/* OTA-Upgrade-Callback – in zb_action_handler aufrufen */
esp_err_t zb_ota_client_handle(const void *message);
