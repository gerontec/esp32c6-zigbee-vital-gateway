#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Zigbee-Koordinator starten */
void zb_gateway_start(void);

/* Permit Join für N Sekunden öffnen (0 = sofort schließen) */
void zb_gateway_permit_join(uint8_t seconds);

/* Geräteliste ins Log schreiben (Debug) */
void zb_gateway_list_devices(void);

/* Geräteliste als JSON-Array in buf schreiben (thread-safe) */
void zb_gateway_devices_json(char *buf, size_t len);

/* Zigbee-Kanal setzen (11-26), speichert in NVS und startet neu.
 * Default: 20, Fallback: 25 (beim nächsten Neustart wirksam). */
void zb_gateway_set_channel(uint8_t ch);

/* Alle Kanäle 11-26 scannen (~4s/Kanal), Ergebnisse als JSON ausgeben */
void zb_gateway_scan_channels(void);
