#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "mr60bha1.h"

/* MQTT-Verbindung aufbauen */
esp_err_t ha_mqtt_init(const char *broker_uri,   /* z.B. "mqtt://192.168.1.10" */
                       const char *username,      /* oder NULL */
                       const char *password);     /* oder NULL */

/* Vitaldaten vom MR60BHA1 publizieren + HA-Discovery registrieren */
void ha_mqtt_publish_vitals(const mr60_data_t *d);

/* Generischen Zigbee-Gerätezustand publizieren */
void ha_mqtt_publish_zigbee(uint16_t short_addr,
                            const char *cluster,   /* z.B. "on_off" */
                            const char *payload);  /* JSON-String */

/* Permit-Join-Status publizieren */
void ha_mqtt_publish_permit_join(bool open, uint8_t seconds);

/* True sobald verbunden */
bool ha_mqtt_connected(void);
