#pragma once
/* BLE UART Log Bridge (Nordic UART Service)
 *
 * Registriert NUS-GATT-Service neben Matter-BLE.
 * Notebook verbindet sich mit bluetoothctl/bleak und
 * empfängt ESP-Logs als BLE-Notifications auf TX-Characteristic.
 *
 * NUS UUIDs:
 *   Service : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   TX (ESP→PC, NOTIFY) : 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX (PC→ESP, WRITE)  : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
 */

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Muss VOR esp_matter::start() aufgerufen werden.
 * Registriert NUS-GATT-Service und hängt sich in esp_log_set_vprintf ein.
 */
esp_err_t ble_log_init(void);

/** Direkte Ausgabe an verbundenen BLE-Client (max 512 Bytes). */
void ble_log_write(const char *buf, size_t len);

#ifdef __cplusplus
}
#endif
