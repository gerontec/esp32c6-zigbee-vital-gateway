#pragma once
#include <stdbool.h>

/* Timeout in Sekunden: 1 min für Tests, 600 für Produktion */
#define WIFI_SWITCH_TIMEOUT_S  60

/* NVS-Flag gesetzt? (prüfen in app_main vor Zigbee-Start) */
bool wifi_switch_is_pending(void);

/* Flag setzen + sofortiger Neustart → nächster Boot läuft im WiFi-Modus */
void wifi_switch_trigger(void);

/* WiFi-Modus ausführen (blockiert bis esp_restart) – nur in app_main aufrufen */
void wifi_switch_run(void);
