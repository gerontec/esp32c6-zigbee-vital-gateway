# Development Findings – ESP32-C6 Zigbee Vital Gateway

## Hardware Setup

### Coordinator (Raspberry Pi)
- **ttyACM0** = CH340 USB-UART (1a86:55d3) → GPIO16/17 of coordinator ESP32-C6
- **ttyACM1** = Espressif USB-JTAG/CDC (303a:1001) → native USB of coordinator ESP32-C6
- udev symlinks: `zigbee-coordinator` → ttyACM0, `zigbee-coordinator-jtag` → ttyACM1
- JSON output flows through **ttyACM1** (USB-JTAG), via `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`

### Client (Notebook)
- **ttyACM0** = Espressif USB-JTAG/CDC (303a:1001) → native USB of client ESP32-C6
- Client MAC (IEEE 802.15.4): `98:a3:16:ff:fe:97` (EUI-64: `98:a3:16:ff:fe:97:c4:10`)
- Client Zigbee short address: `0xb4c9` (changes on rejoin; use IEEE for stable identification)
- DS18B20: GPIO6 (4.7 kΩ pull-up), uses `espressif/onewire_bus` RMT component

---

## Critical Findings & Lessons Learned

### 1. DTR/RTS Reset Problem
Manual DTR=True + RTS=False combination puts ESP32-C6 into download mode (`boot:0x4 DOWNLOAD`).

**Fix:** Always use `esptool --before default_reset`. Never manually toggle DTR/RTS for reset.

---

### 2. USB Port Direction (ttyACM0 vs ttyACM1)
- ttyACM0 = CH340 = UART0
- ttyACM1 = Espressif native USB-JTAG = stdout when `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
- **JSON travels on ttyACM1**; Pi's `serial_gateway.py` reads from `/dev/zigbee-coordinator-jtag`

---

### 3. Stack Overflow in Heartbeat Task
`Guru Meditation Error: Stack protection fault in task "heartbeat"` — task needed >2048 bytes
for `char lqi_buf[256]` + `char line[400]` + nested calls.

**Fix:** heartbeat task stack 2048 → **4096 bytes**.

---

### 4. `esp_ieee802154_get_recent_rssi()` Called Too Early
Called before Zigbee initialized → crash on first heartbeat fire.

**Fix (coordinator):** Added `static volatile int8_t s_last_rssi` updated in `aps_data_ind_cb`;
replaced direct call with `zb_gateway_get_last_rssi()`.

---

### 5. ZCL API Thread Safety — `esp_zb_zcl_report_attr_cmd_req()` Crashes from FreeRTOS Task
Any `esp_zb_zcl_*` API **must** run in the Zigbee task context (the `esp_zb_main_loop_iteration()` thread).
There is no `esp_zb_lock_acquire()` in ESP-IDF Zigbee v1.0.9 / ESP-IDF v5.2.5.

**Fix:** Schedule via `esp_zb_scheduler_alarm()` started FROM within the Zigbee context
(in `esp_zb_app_signal_handler` on the STEERING join signal). Callback reschedules itself every 65 s.
The FreeRTOS heartbeat task only writes to a `volatile int16_t` (atomic on RISC-V).

```c
// In STEERING signal handler (Zigbee context):
esp_zb_scheduler_alarm(temp_report_periodic, 0, TEMP_REPORT_MS);

// Periodic callback (Zigbee context):
static void temp_report_periodic(uint8_t param) {
    int16_t t = s_temp_pending;          // read volatile
    esp_zb_zcl_report_attr_cmd_req(&cmd);
    esp_zb_scheduler_alarm(temp_report_periodic, 0, TEMP_REPORT_MS);
}

// From heartbeat task (any context):
void zb_device_report_temp(int16_t temp_100) {
    s_temp_pending = temp_100;           // atomic int16 write
}
```

---

### 6. ZCL Attribute Reports Silently Dropped — Unregistered Cluster
Client sends ZCL "Report Attributes" for Temperature Measurement (0x0402) to coordinator EP1.
Coordinator EP1 had no temperature cluster registered → ZCL layer silently dropped the frame.
`ESP_ZB_CORE_REPORT_ATTR_CB_ID` never fired.

**Fix:** Register Temperature Measurement **as CLIENT role** on coordinator EP1:
```c
esp_zb_cluster_list_add_temperature_meas_cluster(cl,
    esp_zb_temperature_meas_cluster_create(&temp_cfg),
    ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
```

---

### 7. Permanent Device Identity: IEEE MAC vs Zigbee Short Address
Zigbee short address changes on every rejoin. Use `esp_read_mac(mac, ESP_MAC_IEEE802154)`
for the permanent 6-byte MAC. Format: `%02x:%02x:%02x:%02x:%02x:%02x` → `98:a3:16:ff:fe:97`.

---

### 8. Temperature 0x8000 = "Invalid/Unknown" in ZCL
ZCL Temperature Measurement: `MeasuredValue = 0x8000` (int16_t = -32768) means no valid reading.

**Normalization in `serial_gateway.py`:**
```python
def _extract_value(cluster, raw):
    if cluster == "temperature":
        r = raw.get("raw")
        return None if (r is None or r == -32768) else r / 100.0
```
Also normalize `p["raw"] = None` before storing the cluster payload.

---

### 9. Service Architecture
- **Pi (192.168.178.218):** `zigbee-mqtt-gateway.service` reads coordinator ttyACM1, web UI port 8082
- **Notebook:** No service — only used for flashing client ESP32-C6 via ttyACM0
- Temperature data path: Client DS18B20 → Zigbee ZCL attr report (0x0402) → Coordinator → serial_gateway.py → MariaDB + web UI

---

### 10. esptool Syntax Difference
- **Pi (older esptool 4.x):** underscore syntax: `--before default_reset`, `write_flash`, `--flash_mode`
- **Notebook (newer esptool 5.x):** hyphen syntax: `--before default-reset`, `write-flash`, `--flash-mode`

---

## Current Heartbeat JSON Formats

### Client (USB ttyACM0 → notebook):
```json
{"t":"heartbeat","mac":"98:a3:16:ff:fe:97","uptime":61,"pan":"0x43cb","ch":20,"rssi":-85,"temp":null}
```

### Client (Zigbee ZCL Temperature Cluster → coordinator → serial_gateway.py):
- No sensor: `{"raw": null}` (after normalization from -32768)
- With sensor: `{"raw": 2250}` = 22.50 °C

### Coordinator gateway heartbeat (ttyACM1 → Pi):
```json
{"t":"heartbeat","uptime":6005,"ch":20,"pan":"0x43cb","rssi":-34,"dev":[{"addr":"0xb4c9","lqi":0,"rssi":-88},{"addr":"0x0684","lqi":234,"rssi":-34}]}
```

---

## Build & Flash Workflow

```bash
# Client firmware (build on Pi, flash from notebook):
ssh pi 'cd ~/python/esp32c6-zigbee-vital-gateway/client && source ~/esp-idf/export.sh && idf.py build'
scp pi:~/python/esp32c6-zigbee-vital-gateway/client/build/zigbee-client.bin /tmp/
esptool --chip esp32c6 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m \
  0x20000 /tmp/zigbee-client.bin

# Coordinator firmware (build and flash on Pi):
ssh pi 'cd ~/python/esp32c6-zigbee-vital-gateway && source ~/esp-idf/export.sh && idf.py build'
ssh pi 'sudo systemctl stop zigbee-mqtt-gateway'
ssh pi 'python -m esptool --chip esp32c6 -p /dev/ttyACM1 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x20000 build/zigbee-vital-sensor.bin'
ssh pi 'sudo systemctl start zigbee-mqtt-gateway'
```
