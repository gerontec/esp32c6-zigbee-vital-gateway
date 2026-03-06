#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── Gerätebeschreibung (für Web-UI und externe Abfragen) ─────────────────── */
#define ZB_DEVICE_NAME_MAX 32

typedef struct {
    uint16_t short_addr;
    uint8_t  ieee[8];
    char     name[ZB_DEVICE_NAME_MAX];  /* frei wählbarer Anzeigename */
    bool     used;
} zb_device_info_t;

/* Zigbee-Koordinator starten */
void zb_gateway_start(void);

/* Permit Join für N Sekunden öffnen (0 = schließen) */
void zb_gateway_permit_join(uint8_t seconds);

/* Alle gepairten Geräte ausgeben (Log) */
void zb_gateway_list_devices(void);

/* Geräteliste in out[] kopieren; gibt Anzahl der Einträge zurück (max = Puffergröße) */
int zb_gateway_get_devices(zb_device_info_t *out, int max);

/* Anzeigename für ein Gerät setzen (bleibt bis Neustart erhalten) */
void zb_gateway_set_name(uint16_t short_addr, const char *name);
