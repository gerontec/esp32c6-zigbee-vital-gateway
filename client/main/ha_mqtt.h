#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * ha_mqtt – USB Serial JTAG JSON-Lines Bridge (Client/Router)
 *
 * C6 → Pi: {"t":"boot"}
 *           {"t":"joined","pan":"0x<hex>","ch":<n>}
 *           {"t":"left"}
 *           {"t":"attr","cluster":"<name>","v":<val>}
 *           {"t":"heartbeat","uptime":<s>,"pan":"0x<hex>","ch":<n>}
 * Pi → C6: {"cmd":"leave"}
 *           {"cmd":"set_onoff","v":0|1}
 *           {"cmd":"permit_join","sec":<n>}   (nur Router)
 */

typedef void (*mqtt_cmd_cb_t)(const char *cmd, const char *payload, int len);

esp_err_t   ha_mqtt_init(void);
void        ha_mqtt_set_cmd_cb(mqtt_cmd_cb_t cb);

void        ha_mqtt_publish_boot(void);
void        ha_mqtt_publish_joined(uint16_t pan_id, uint8_t channel);
void        ha_mqtt_publish_left(void);
void        ha_mqtt_publish_attr(const char *cluster, const char *json_val);
void        ha_mqtt_publish_heartbeat(uint32_t uptime_s, uint16_t pan_id, uint8_t channel, int8_t rssi);
void        ha_mqtt_publish_ota_status(const char *status, uint32_t offset, uint32_t total);
