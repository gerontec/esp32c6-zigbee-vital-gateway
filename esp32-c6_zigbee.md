# ESP32-C6 Zigbee Gateway – Systembeobachtungen

Dokumentiert die tatsächliche Hardware- und Software-Architektur des laufenden Systems
(Stand: 2026-03-22, Analyse über SSH/MQTT-Monitoring).

---

## Hardware

### Gerät
- **ESP32-C6 DevKit** (QFN40, Revision v0.2)
- Features: Wi-Fi 6, BT 5 (LE), IEEE 802.15.4, Single Core + LP Core, 160 MHz, 40 MHz Crystal
- **MAC:** `b0:a6:04:87:10:e0`

### Zwei USB-Ports am Devboard

| Port (Pi-Seite) | USB-Chip | ESP32-C6-Seite | Verwendung |
|-----------------|----------|----------------|------------|
| `/dev/ttyACM0` | CH340 (`usb-1a86_USB_Single_Serial`) | UART0 (Hardware-Brücke) | **Nur Flashen** (esptool) |
| `/dev/ttyACM1` | USB-JTAG (`usb-Espressif_USB_JTAG_serial_debug_unit`) | USB-JTAG / USB-CDC (built-in) | **Datenkommunikation** (laufender Betrieb) |

> **Wichtig:** `ttyACM0` (CH340) ist im laufenden Betrieb stumm – kein Output.  
> Alle JSON-Daten kommen ausschließlich über `ttyACM1` (USB-JTAG).

---

## Firmware-Kommunikation (`ha_mqtt.c`)

### Design-Entscheidung
```c
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y   // IDF-Console = USB-JTAG-Port
esp_log_set_vprintf(null_vprintf)       // alle ESP_LOGI vollständig stumm
printf() / fflush(stdout)              // nur sauberes JSON über stdout → ttyACM1
```

- **Kein Debug-Müll** auf der Leitung – ausschließlich JSON-Lines
- `ESP_LOGI`-Ausgaben werden in `null_vprintf` verworfen (inkl. der Boot-Kommentare in `main.c`)
- Der Kommentar in `main.c` (`UART1 TX=GPIO16 RX=GPIO17`) ist **veraltet** – UART1 wird nicht benutzt

### Protokoll C6 → Pi (stdout → ttyACM1)

| Message | JSON |
|---------|------|
| Boot | `{"t":"boot"}` |
| Heartbeat (alle 10 s) | `{"t":"heartbeat","uptime":<s>,"ch":<n>,"pan":"0x<hex>"}` |
| Zigbee-Daten | `{"t":"zigbee","addr":<uint>,"sub":"<name>","p":{...}}` |
| Permit Join Status | `{"t":"permit_join","p":{"open":true/false,"seconds":<n>}}` |

### Protokoll Pi → C6 (stdin ← ttyACM1)

| Kommando | JSON |
|----------|------|
| Permit Join öffnen | `{"cmd":"permit_join","sec":<n>}` |
| Kanal wechseln | `{"cmd":"set_channel","ch":<11-26>}` |

---

## Systemd-Service

```ini
# /etc/systemd/system/zigbee-mqtt-gateway.service
ExecStart=/usr/bin/python3 -u \
    /home/pi/python/esp32c6-zigbee-vital-gateway/host/serial_gateway.py \
    --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_B0:A6:04:87:10:E0-if00 \
    --broker 192.168.178.218 \
    --base gw/coordinator
```

`serial_gateway.py` liest von `ttyACM1` und bridged nach MQTT.

---

## MQTT-Topics (`gw/coordinator/...`)

| Topic | Inhalt | Quelle |
|-------|--------|--------|
| `gw/coordinator/status` | `online` / `offline` (LWT) | Service-Start / Disconnect |
| `gw/coordinator/heartbeat` | `{"t":"heartbeat","uptime":...,"ch":...,"pan":"..."}` | Jede Heartbeat-Zeile von ttyACM1 |
| `gw/coordinator/console` | identisch mit heartbeat-Payload (roh) | **jede** Zeile von ttyACM1 |
| `gw/coordinator/zigbee/0x<addr>/<sub>` | Sensor-Daten | Zigbee-Events |
| `gw/coordinator/permit_join` | `{"open":...,"seconds":...}` | Permit-Join-Status |

### Bekanntes Problem: `console` = `heartbeat`

In `serial_gateway.py` / `dispatch()`:
```python
def dispatch(line: str):
    mq.publish(f"{BASE}/console", line, qos=0)   # ← jede Zeile roh publizieren
    ...
    elif t == "heartbeat":
        mq.publish(f"{BASE}/heartbeat", line, retain=True, qos=1)  # ← nochmal
```

Da der C6 **nur** strukturiertes JSON sendet (kein Debug-Output), landen Heartbeats
doppelt – auf `console` und `heartbeat` mit identischem Payload.

**Fix:** `mq.publish(f"{BASE}/console", line)` aus `dispatch()` entfernen,
oder nur Nicht-Heartbeat-Zeilen auf `console` publishen.

---

## Flash-Partitionen

Ausgelesen mit `esptool` von `ttyACM0` (Offset `0x8000`):

| Name | Typ | SubTyp | Offset | Größe |
|------|-----|--------|--------|-------|
| `nvs` | data | nvs | `0x9000` | 24 KB |
| `otadata` | data | ota | `0xf000` | 8 KB |
| `phy_init` | data | phy | `0x11000` | 4 KB |
| `ota_0` | app | ota_0 | `0x20000` | 1,75 MB |
| `ota_1` | app | ota_1 | `0x1e0000` | 1,75 MB |
| `zb_storage` | data | fat | `0x3a0000` | 512 KB |
| `zb_fct` | data | fat | `0x3a8000` | 64 KB |

- OTA-fähig (zwei App-Slots ota_0 / ota_1)
- Zigbee-Netzwerk-Persistenz in `zb_storage` (FAT, 512 KB)
- Zigbee Factory-Calibration in `zb_fct` (FAT, 64 KB)

---

## Aktive MQTT-Gateways (Überblick)

```
mosquitto_sub -h 192.168.178.218 -t 'gw/#' -v
```

| Base-Topic | Gerät | Script | Status |
|------------|-------|--------|--------|
| `gw/coordinator` | ESP32-C6 DevKit (ttyACM1) | `serial_gateway.py` (Service) | ✅ aktiv |
| `gw/c6serial` | ESP32-C6 DevKit (ttyACM1) | `c6_serial_gw.py` (kein Service) | ⚠️ veraltet |
| `gw/c6` | ESP32-C6 DevKit (ttyACM0) | `c6_serial_gw.py` | ❌ offline |
| `gw/client` | Zigbee-Client-Firmware | — | offline |

