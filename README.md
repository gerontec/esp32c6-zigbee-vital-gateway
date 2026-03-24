# ESP32-C6 Zigbee Vital Signs Gateway

Split-architecture Zigbee gateway: **ESP32-C6** acts as Zigbee coordinator (USB-serial to Raspberry Pi), **Raspberry Pi** runs `serial_gateway.py` bridging to MQTT and providing the Web UI.

```
Zigbee devices ──802.15.4──► ESP32-C6 (coordinator)
                                   │  USB/UART
                              Raspberry Pi
                              serial_gateway.py
                                   │
                          MQTT Broker (Mosquitto)
                                   │
                          Home Assistant / any MQTT client
                          Web UI :8082
```

## Hardware

| Component | Notes |
|---|---|
| ESP32-C6 DevKit | Zigbee coordinator (USB to Pi) |
| Seeed MR60BHA2 | 60 GHz mmWave radar, UART1, optional |
| Raspberry Pi 3/4/5 | Runs serial_gateway.py as systemd service |

### Wiring (MR60BHA2 → ESP32-C6, optional)

| MR60BHA2 | ESP32-C6 |
|---|---|
| TX | GPIO 5 (RX1) |
| RX | GPIO 4 (TX1) |
| GND | GND |
| 5V | VIN |

---

## Quick Start

### 1. Build & Flash Coordinator

```bash
source ~/esp-idf/export.sh
cd esp32c6-zigbee-vital-gateway
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

### 2. Build & Flash Client (optional sensor node)

```bash
source ~/esp-idf/export.sh
cd esp32c6-zigbee-vital-gateway/client
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

### 3. Start serial_gateway.py on Raspberry Pi

```bash
python3 host/serial_gateway.py \
  --port /dev/ttyUSB0 \
  --broker 192.168.178.1 \
  --base gw/coordinator
```

Or use the included systemd service:

```bash
sudo cp esp32gw.service /etc/systemd/system/
sudo systemctl enable --now esp32gw.service
```

Web UI: `http://<pi-ip>:8082/`

---

## MQTT Topics

Base topic default: `gw/coordinator` (configurable via `--base`)

### Published by Gateway

| Topic | Content | Notes |
|---|---|---|
| `gw/coordinator/status` | `online` / `offline` | LWT, retained |
| `gw/coordinator/heartbeat` | `{"t":"heartbeat","uptime":…}` | Retained, every ~10 min |
| `gw/coordinator/console` | raw UART log line | Debug |
| `gw/coordinator/zigbee/0xADDR/temperature` | `{"temperature":21.50}` | °C |
| `gw/coordinator/zigbee/0xADDR/humidity` | `{"humidity":55.00}` | % RH |
| `gw/coordinator/zigbee/0xADDR/illuminance` | `{"lux":320.5}` | lux |
| `gw/coordinator/zigbee/0xADDR/occupancy` | `{"occupancy":true}` | PIR / radar |
| `gw/coordinator/zigbee/0xADDR/ias_zone` | `{"zone_status":1,"motion":1}` | IAS Zone sensors |
| `gw/coordinator/zigbee/0xADDR/battery` | `{"pct":85}` | Battery % |
| `gw/coordinator/zigbee/0xADDR/on_off` | `{"state":"ON"}` | On/Off cluster |
| `gw/coordinator/zigbee/0xADDR/raw` | raw JSON | Unknown clusters |
| `gw/coordinator/zigbee/0xADDR/status` | `{"event":"joined"}` | Join/leave events |
| `gw/coordinator/mr60bha2` | `{"bpm":72,"rpm":16,…}` | Radar vital signs |
| `gw/coordinator/permit_join` | `{"open":true,"seconds":180}` | Network state |

### Command Topics (publish to these)

| Topic | Payload | Action |
|---|---|---|
| `gw/coordinator/cmd/permit_join` | `180` (seconds, integer) | Open Zigbee network for joining |
| `gw/coordinator/cmd/set_channel` | `20` (channel 11–26) | Change Zigbee channel (coordinator restarts) |
| `gw/coordinator/cmd/scan_chan` | *(empty)* | Scan all channels, report energy |
| `gw/coordinator/cmd/switch2wifi` | *(empty)* | Send switch2wifi to **all** connected client nodes |
| `gw/coordinator/cmd/set_sleep` | `600` (seconds, min 10) | Set heartbeat/sleep interval on all client nodes |

#### Examples

```bash
MQTT_BASE="gw/coordinator"
BROKER="192.168.178.1"

# Open network for 3 minutes
mosquitto_pub -h $BROKER -t "$MQTT_BASE/cmd/permit_join" -m 180

# Set Zigbee channel 25 (avoids 2.4 GHz WiFi ch 1,6,11)
mosquitto_pub -h $BROKER -t "$MQTT_BASE/cmd/set_channel" -m 25

# Scan channel energy
mosquitto_pub -h $BROKER -t "$MQTT_BASE/cmd/scan_chan" -m ""

# Tell all client nodes to try WiFi (fallback to Zigbee after 60 s)
mosquitto_pub -h $BROKER -t "$MQTT_BASE/cmd/switch2wifi" -m ""

# Change client sleep cycle to 5 minutes (300 s)
mosquitto_pub -h $BROKER -t "$MQTT_BASE/cmd/set_sleep" -m 300
```

---

## Web UI

Open `http://<pi-ip>:8082/` in a browser.

| Page | URL | Description |
|---|---|---|
| Overview | `/` | All devices, latest values, permit-join button |
| Device detail | `/device/0xADDR` | All clusters, last payloads, command buttons, sleep form |

### Web UI Commands (per device)

- **switch2wifi** – trigger WiFi switch on that device
- **leave** – remove device from Zigbee network
- **Sleep-Intervall** – set heartbeat interval (seconds) via POST `/api/sleep`

### REST API

```bash
PI="192.168.178.218:8082"

# Open permit-join for 180 s (Web UI button)
curl -X POST http://$PI/api/permit_join -d "sec=180"

# Send command to a specific device
curl -X POST http://$PI/api/device/0x1234/cmd -d "cmd=switch2wifi"
curl -X POST http://$PI/api/device/0x1234/cmd -d "cmd=leave"

# Set sleep interval for all clients (broadcasts via MQTT → coordinator → ZCL)
curl -X POST http://$PI/api/sleep -d "secs=600"
```

---

## Supported Zigbee Clusters (CLIENT on coordinator)

| Cluster | ID | MQTT subtopic |
|---|---|---|
| On/Off | 0x0006 | `on_off` |
| Temperature Measurement | 0x0402 | `temperature` |
| Relative Humidity | 0x0405 | `humidity` |
| Illuminance Measurement | 0x0400 | `illuminance` |
| Occupancy Sensing | 0x0406 | `occupancy` |
| IAS Zone | 0x0500 | `ias_zone` |
| Power Configuration | 0x0001 | `battery` |
| Custom (0xFF01) | 0xFF01 | – (commands only) |

---

## Client Firmware (ESP32-C6 Sensor Node)

The `client/` subdirectory contains firmware for standalone ESP32-C6 nodes that:
- Join the Zigbee network and report temperature via ZCL
- Receive ZCL custom commands on cluster 0xFF01:
  - `cmd 0x01` – switch to WiFi mode (scans open APs, connects, timeout 60 s, then restarts into Zigbee)
  - `cmd 0x02` – set sleep/heartbeat interval (uint32 seconds, persisted in NVS)
- Store sleep interval in NVS (`client_cfg` / `sleep_s`), survives reboot

### Client Sleep Interval

Default: **600 seconds** (10 minutes). Minimum: 10 s. Set via:

```bash
# Via MQTT (broadcasts to all clients)
mosquitto_pub -h 192.168.178.1 -t "gw/coordinator/cmd/set_sleep" -m 300

# Via Web UI: device detail page → Sleep-Intervall form
```

---

## Channel Recommendations

| Zigbee Channel | Center Freq | Avoids WiFi |
|---|---|---|
| **15** | 2425 MHz | WLAN 1, 6 |
| **20** | 2450 MHz | WLAN 1, 6, 11 (partial) |
| **25** | 2475 MHz | WLAN 1, 6, 11 ✓ |

Default network channel: **20**. Tuya devices prefer 11, 15, 20, 25.

---

## Project Structure

```
esp32c6-zigbee-vital-gateway/
├── main/                       # Coordinator firmware (ESP32-C6)
│   ├── main.c                  # app_main, UART cmd dispatcher
│   ├── zb_gateway.c/.h         # Zigbee coordinator, ZCL dispatch, ZCL custom cmds
│   ├── ha_mqtt.c/.h            # UART→MQTT bridge, command parser
│   ├── mr60bha2.c/.h           # MR60BHA2 radar driver
│   └── zb_ota_server.c/.h      # OTA server
├── client/main/                # Sensor node firmware (ESP32-C6)
│   ├── main.c                  # app_main, heartbeat task, NVS sleep interval
│   ├── zb_device.c/.h          # Zigbee end-device, ZCL custom cmd handler
│   ├── ha_mqtt.c/.h            # UART emit helpers
│   ├── wifi_switch.c/.h        # WiFi scan + connect, 60 s timeout, restart
│   └── zb_ota_client.c/.h      # OTA client
└── host/
    └── serial_gateway.py       # Pi bridge: UART↔MQTT, Web UI :8082, MariaDB
```

---

## License

MIT
