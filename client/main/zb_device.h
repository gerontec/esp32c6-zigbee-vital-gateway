#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Zigbee Router starten – verbindet sich mit bestehendem Netz */
void zb_device_start(void);

/* On/Off Attribut setzen und reporten */
void zb_device_set_onoff(bool on);

/* Permit Join für N Sekunden öffnen (Router kann auch pairen) */
void zb_device_permit_join(uint8_t seconds);

/* Netzwerk verlassen */
void zb_device_leave(void);

/* Kanal setzen (NVS + Neustart, identisch zum Coordinator) */
void zb_device_set_channel(uint8_t ch);

/* Aktuellen Netzwerkstatus */
bool     zb_device_joined(void);
uint16_t zb_device_pan_id(void);
uint8_t  zb_device_channel(void);
