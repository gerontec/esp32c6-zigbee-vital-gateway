#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_zigbee_core.h"

/*
 * zb_ota_server – Zigbee OTA Upgrade Server (Coordinator-Seite)
 *
 * Protokoll Pi→C6: {"cmd":"ota_start","size":<N>,"ver":<V>}
 *                  {"cmd":"ota_data","off":<O>,"data":"<HEX>"}
 * Protokoll C6→Pi: {"t":"ota_req","off":<O>,"sz":<S>}
 *                  {"t":"ota_status","status":"started"|"error","size":<N>}
 *                  {"t":"ota_srv_status","addr":"0x<A>","status":"started"|"aborted"|"done"}
 */

/* Erstellt das OTA-Server-Attribut-Cluster (SERVER_ROLE) */
esp_zb_attribute_list_t *zb_ota_server_cluster_create(void);

/* Startet OTA-Session nach ota_start-Kommando vom Pi */
void zb_ota_server_start(uint32_t size, uint32_t version);

/* Liefert den nächsten Daten-Chunk (hex-kodiert, byte_count = Anzahl der Bytes) */
void zb_ota_server_feed_data(const char *hex, uint8_t byte_count);
