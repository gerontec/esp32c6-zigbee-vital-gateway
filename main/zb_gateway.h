#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Zigbee-Koordinator starten */
void zb_gateway_start(void);

/* Permit Join für N Sekunden öffnen (0 = sofort schließen) */
void zb_gateway_permit_join(uint8_t seconds);

/* Geräteliste ins Log schreiben (Debug) */
void zb_gateway_list_devices(void);
