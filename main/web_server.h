#pragma once
#include "esp_err.h"

/* HTTP-Server auf Port 80 starten.
 * Bietet:
 *   GET  /              – HTML-Übersicht aller Zigbee-Geräte + Vitalwerte
 *   GET  /api/devices   – Geräteliste als JSON
 *   GET  /api/vitals    – aktuelle MR60BHA1-Werte als JSON
 *   POST /api/device    – Gerät umbenennen  (body: addr=0x1234&name=Kueche)
 */
esp_err_t web_server_start(void);
