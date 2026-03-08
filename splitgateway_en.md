# Split-Gateway Design: ESP32-C6 Minimal + Linux Host Intelligent

## Architecture Overview

```
┌─────────────────────────────────┐       MQTT        ┌────────────────────────────────┐
│        ESP32-C6 (C6)            │ ─────────────────▶ │    Linux Host (Pi / Server)    │
│        Radio Bridge             │                    │    gateway_service.py          │
│                                 │ ◀───────────────── │                                │
│  ┌──────────────────────────┐   │  gw/<mac>/cmd/+   │  ✓ HA Auto-Discovery           │
│  │ Zigbee Coordinator       │   │                    │  ✓ Device name persistence     │
│  │ (802.15.4 Radio)         │   │                    │  ✓ Web dashboard port 8080     │
│  └──────────────────────────┘   │                    │  ✓ REST API for control        │
│  ┌──────────────────────────┐   │                    │  ✓ JSON logging                │
│  │ MR60BHA1 UART driver     │   │                    └────────────────────────────────┘
│  └──────────────────────────┘   │                              │
│  ┌──────────────────────────┐   │                              │ MQTT
│  │ MQTT Bridge (no HA)      │   │                              ▼
│  └──────────────────────────┘   │                    ┌────────────────────────────────┐
└─────────────────────────────────┘                    │     Mosquitto Broker           │
                                                        │     + Home Assistant           │
```

---

## Layer 1 – ESP32-C6 (Radio Bridge, as thin as possible)

### What the C6 does
| Task | File |
|------|------|
| Zigbee Coordinator (ZCL dispatch → raw MQTT) | `zb_gateway.c` |
| MR60BHA1 UART → raw MQTT | `mr60bha1.c` + `ha_mqtt.c` |
| MQTT publish/subscribe | `ha_mqtt.c` |
| Permit-Join via MQTT command | `main.c` + `ha_mqtt.c` |
| Permit-Join via boot button (fallback) | `main.c` |

### What the C6 no longer does
- ❌ HA Auto-Discovery
- ❌ HTTP web server
- ❌ Device name database
- ❌ String categories (raw integers only)
- ❌ Any logic beyond forwarding sensor data

---

## MQTT Topic Schema

### Publish (C6 → Host)

| Topic | Payload | Description |
|-------|---------|-------------|
| `gw/<mac>/status` | `"online"` / `"offline"` | LWT + connect announce |
| `gw/<mac>/mr60bha1` | `{"bpm":72,"rpm":16,"bpm_cat":1,"rpm_cat":1,"status":2,"bpm_wave":0.85,"rpm_wave":0.42}` | Vital signs (raw integers) |
| `gw/<mac>/zigbee/0x1234/on_off` | `{"v":1}` | 1=ON, 0=OFF |
| `gw/<mac>/zigbee/0x1234/temperature` | `{"raw":2150}` | 1/100 °C → host divides |
| `gw/<mac>/zigbee/0x1234/humidity` | `{"raw":5500}` | 1/100 % → host divides |
| `gw/<mac>/zigbee/0x1234/illuminance` | `{"raw":320}` | Lux |
| `gw/<mac>/zigbee/0x1234/occupancy` | `{"occ":1}` | 1=occupied, 0=clear |
| `gw/<mac>/zigbee/0x1234/join` | `{"event":"joined","addr":"0x1234","ieee":"01:02:03:04:05:06:07:08"}` | Device joined |
| `gw/<mac>/zigbee/0x1234/raw` | `{"cluster":"0x0300","attr":"0x0000"}` | Unknown clusters |
| `gw/<mac>/permit_join` | `{"open":true,"seconds":180}` | Pairing window status |

### Subscribe (Host → C6)

| Topic | Payload | Description |
|-------|---------|-------------|
| `gw/<mac>/cmd/permit_join` | `"180"` or `"0"` | Open/close join window |

### Category mapping (bpm_cat / rpm_cat)
| Value | Meaning |
|-------|---------|
| 0 | none (no signal) |
| 1 | normal |
| 2 | fast |
| 3 | slow |

### Status mapping (status)
| Value | Meaning |
|-------|---------|
| 0 | init |
| 1 | calibrating |
| 2 | measuring |

---

## Layer 2 – Linux Host (gateway_service.py)

### Features
- **MQTT subscriber**: listens on `gw/+/#` (all bridge instances, multi-C6 capable)
- **HA Auto-Discovery**: publishes `homeassistant/sensor/.../config` automatically
  - On first `online` status → MR60BHA1 entities
  - On first packet per Zigbee cluster → Zigbee entity
- **Device name persistence**: stored in `devices.json`
- **Web dashboard**: port 8080, auto-refresh every 10 s
- **REST endpoints**:
  - `POST /api/permit_join` → sends MQTT command to C6
  - `POST /api/rename` → saves device friendly name
  - `GET  /api/state` → full state as JSON

### Configuration (environment variables)
| Variable | Default | Description |
|----------|---------|-------------|
| `MQTT_HOST` | `localhost` | Mosquitto broker IP |
| `MQTT_PORT` | `1883` | Mosquitto port |
| `MQTT_USER` | empty | MQTT username (optional) |
| `MQTT_PASS` | empty | MQTT password (optional) |
| `WEB_PORT` | `8080` | Web dashboard port |
| `DEVICES_FILE` | `$HOME/esp32-gw/devices.json` | Device name persistence |
| `LOG_LEVEL` | `INFO` | Logging level |

---

## Installation on the Linux Host

### Native install (no Docker, no sudo required)

```bash
# Copy host files to the target machine
scp -r host/ pi@<host-ip>:~/esp32-gw/

# Run the installer
ssh pi@<host-ip> 'cd ~/esp32-gw && bash install.sh'
```

`install.sh` requires only a regular user account (no `sudo`):
1. Creates a Python venv in `~/esp32-gw/venv/`
2. Installs `paho-mqtt` into the venv
3. Writes a `systemd --user` service unit
4. Enables the service and starts it immediately
5. Calls `loginctl enable-linger` so the service survives after logout

**Prerequisites** (install once with sudo if not present):
```bash
sudo apt install python3-venv mosquitto mosquitto-clients
```

### Service management
```bash
# Status
systemctl --user status esp32gw
journalctl --user -fu esp32gw

# Restart / stop
systemctl --user restart esp32gw
systemctl --user stop esp32gw

# Permit join manually (without web UI)
mosquitto_pub -t gw/<mac>/cmd/permit_join -m 180
mosquitto_pub -t gw/<mac>/cmd/permit_join -m 0

# Monitor all MQTT messages
mosquitto_sub -t 'gw/#' -v
```

### Docker (alternative)

If you prefer containers, a `docker-compose.yml` is included. It uses `network_mode: host` to connect to an already-running Mosquitto broker on `localhost:1883`.

```bash
cd host/
docker compose up -d
```

---

## ESP32-C6 Firmware Configuration

Edit `main/main.c`:
```c
#define WIFI_SSID        "YourNetwork"
#define WIFI_PASSWORD    "YourPassword"
#define MQTT_BROKER_URI  "mqtt://192.168.1.10"   // IP of your broker/Pi
```

Build and flash:
```bash
cd zigbee-vital-sensor
idf.py build flash monitor
```

The base topic is derived automatically from the WiFi MAC address:
```
gw/a1b2c3d4/...
```

---

## Robustness

| Scenario | Behaviour |
|----------|-----------|
| MQTT broker unreachable | ESP32: automatic reconnect; host: automatic reconnect |
| Host service crashes | systemd restarts after 5 s (`RestartSec=5`) |
| C6 loses WiFi | Reconnects; MQTT LWT publishes `offline` |
| C6 reboots | Zigbee network restored from NVS |
| Host restarts | HA Discovery re-sent on next `online` event |
| Permit join without host | Boot button (GPIO9) opens 180 s directly |
| Unknown Zigbee clusters | Published as `raw` topic, no crash |

---

## File Structure

```
esp32c6-zigbee-vital-gateway/
├── main/                         ESP32-C6 firmware (minimal radio bridge)
│   ├── main.c                    WiFi init + MQTT cmd handler + btn_task
│   ├── ha_mqtt.c / .h            MQTT bridge (no HA knowledge)
│   ├── zb_gateway.c / .h         Zigbee coordinator (no device naming)
│   └── mr60bha1.c / .h           MR60BHA1 UART driver (unchanged)
│
├── host/                         Linux host intelligence layer
│   ├── gateway_service.py        Main service (MQTT + web + HA discovery)
│   ├── requirements.txt          paho-mqtt
│   ├── install.sh                Native installer (no sudo, no Docker)
│   ├── esp32gw.service           systemd unit (reference copy)
│   ├── Dockerfile                Docker image definition
│   ├── docker-compose.yml        Docker Compose (alternative to native)
│   └── mosquitto.conf            Example Mosquitto config
│
├── splitgateway.md               Architecture documentation (German)
├── splitgateway_en.md            Architecture documentation (English)
├── CMakeLists.txt                ESP-IDF top-level build
└── sdkconfig.defaults            ESP-IDF build defaults
```
