#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "mr60bha1.h"

/*
 * mqtt_bridge – reine MQTT-Schicht, kein HA-spezifisches Wissen.
 *
 * Basis-Topic:  gw/<MAC4>          (z.B. gw/a1b2c3d4)
 * Pub-Topics:   gw/<base>/status
 *               gw/<base>/mr60bha1
 *               gw/<base>/zigbee/<addr>/<subtopic>
 *               gw/<base>/permit_join
 * Sub-Topics:   gw/<base>/cmd/permit_join   payload: Sekunden (z.B. "180")
 */

/* Callback für eingehende Kommandos.
 *   cmd     – letztes Topic-Segment hinter "cmd/" (z.B. "permit_join")
 *   payload – MQTT-Payload (NICHT null-terminiert, len Bytes)
 *   len     – Payload-Länge
 */
typedef void (*mqtt_cmd_cb_t)(const char *cmd, const char *payload, int len);

/* Verbindung aufbauen */
esp_err_t ha_mqtt_init(const char *broker_uri,
                       const char *username,
                       const char *password);

/* Kommando-Callback registrieren (vor oder nach Init) */
void ha_mqtt_set_cmd_cb(mqtt_cmd_cb_t cb);

/* Rohdaten publizieren */
void ha_mqtt_publish_vitals(const mr60_data_t *d);
void ha_mqtt_publish_zigbee(uint16_t addr,
                             const char *subtopic,
                             const char *payload);
void ha_mqtt_publish_permit_join(bool open, uint8_t seconds);

/* Hilfsfunktionen */
bool        ha_mqtt_connected(void);
const char *ha_mqtt_base_topic(void);   /* gibt "gw/xxxxxxxx" zurück */

/* Debug-Log via MQTT → gw/<mac>/debug  {"tag":"...","msg":"..."} */
void ha_mqtt_logf(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
