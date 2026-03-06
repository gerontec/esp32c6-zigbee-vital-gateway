# ESP32-C6 Zigbee Vital Signs Gateway

An ESP32-C6 firmware that replaces a USB Zigbee dongle (e.g. Sonoff Zigbee 3.0) and adds 60 GHz mmWave vital-sign sensing, all published to Home Assistant via MQTT.

```
MR60BHA1 ──UART──► ESP32-C6 ──WiFi/MQTT──► Home Assistant
Zigbee devices ──802.15.4──► ESP32-C6
                                 │
                            HTTP :80 (device mapping UI)
```

## Features

| Feature | Details |
|---|---|
| Zigbee coordinator | Native 802.15.4 radio on ESP32-C6, no USB dongle needed |
| Vital signs radar | Seeed MR60BHA1 – heartbeat & breathing rate at up to 1.5 m |
| MQTT bridge | All Zigbee sensor data → HA via MQTT, including auto-discovery |
| Web UI | Device mapping on port 80 – assign friendly names, open permit-join |
| REST API | `/api/devices`, `/api/vitals`, `POST /api/device` |

---

## Hardware

| Component | Notes |
|---|---|
| ESP32-C6 DevKit | Any board with exposed UART1 pins |
| Seeed MR60BHA1 | 60 GHz mmWave radar, 3.3 V UART, 5 V supply via VIN |
| Boot button | Already on most DevKits (GPIO9) – triggers permit-join |

### Wiring (MR60BHA1 → ESP32-C6)

| MR60BHA1 pin | ESP32-C6 pin |
|---|---|
| TX | GPIO 5 (RX1) |
| RX | GPIO 4 (TX1) |
| GND | GND |
| 5V | 5V / VIN |

---

## Prerequisites

- [ESP-IDF 5.2+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/)
- Python 3 (comes with ESP-IDF)
- A running MQTT broker reachable from the ESP32-C6 (e.g. Mosquitto inside Home Assistant)

```bash
# Install / activate ESP-IDF (adjust path to your installation)
. ~/esp/esp-idf/export.sh
```

---

## Configuration

Open `main/main.c` and edit the constants at the top:

```c
#define WIFI_SSID        "YourNetwork"
#define WIFI_PASSWORD    "YourPassword"
#define MQTT_BROKER_URI  "mqtt://192.168.1.10"   // IP of your HA MQTT broker
#define MQTT_USER        NULL                     // or "username"
#define MQTT_PASS        NULL                     // or "password"

// MR60BHA1 UART pins
#define MR60_UART        UART_NUM_1
#define MR60_TX_PIN      4    // ESP TX → radar RX
#define MR60_RX_PIN      5    // ESP RX ← radar TX

// Permit-join button (Boot button on most DevKits)
#define BTN_PERMIT_JOIN  9
```

---

## Build & Flash

```bash
# Set target
idf.py set-target esp32c6

# Build
idf.py build

# Flash (adjust port)
idf.py -p /dev/ttyUSB0 flash monitor
```

The first build will automatically fetch the Zigbee SDK components via the IDF component manager (`main/idf_component.yml`).

---

## First Boot

1. The Zigbee coordinator starts and **forms a new network** (factory new).
2. Network steering begins immediately – devices can join.
3. The MQTT client connects and publishes `online` to `vital-gw-XXXXXXXX/status`.
4. The web UI is available at `http://<device-ip>/`.

> The device IP is logged on boot:
> ```
> I (1234) main: IP: 192.168.1.42
> ```

---

## Pairing Zigbee Devices

**Option A – Boot button**
Press the Boot button (GPIO9) on the DevKit. Permit-join opens for 180 seconds.

**Option B – Web UI**
Open `http://<device-ip>/` and click **"180 s öffnen"** in the Permit Join section.

**Option C – MQTT**
Publish to `vital-gw-XXXXXXXX/permit_join` (the status topic accepts open/close events).

Once a device joins, it appears in the web UI and its sensor data is forwarded to MQTT automatically.

---

## MQTT Topics

All topics are prefixed with `vital-gw-<MAC4>` (last 4 bytes of the WiFi MAC, e.g. `vital-gw-a1b2c3d4`).

| Topic | Direction | Content |
|---|---|---|
| `vital-gw-XXXX/status` | pub | `online` / `offline` (LWT) |
| `vital-gw-XXXX/mr60bha1` | pub | Vital signs JSON (see below) |
| `vital-gw-XXXX/zigbee/0xADDR/on_off` | pub | `{"state":"ON"}` |
| `vital-gw-XXXX/zigbee/0xADDR/temperature` | pub | `{"temperature":21.50}` |
| `vital-gw-XXXX/zigbee/0xADDR/humidity` | pub | `{"humidity":55.00}` |
| `vital-gw-XXXX/zigbee/0xADDR/illuminance` | pub | `{"lux":320}` |
| `vital-gw-XXXX/zigbee/0xADDR/occupancy` | pub | `{"occupancy":true}` |
| `vital-gw-XXXX/zigbee/0xADDR/raw` | pub | Unknown clusters as raw JSON |
| `vital-gw-XXXX/zigbee/0xADDR/status` | pub | `{"event":"joined","addr":"0x1234"}` |
| `vital-gw-XXXX/permit_join` | pub | `{"open":true,"seconds":180}` |

### Vital signs payload (`mr60bha1`)

```json
{
  "bpm": 72,
  "rpm": 16,
  "bpm_category": "normal",
  "rpm_category": "normal",
  "status": "messung",
  "bpm_wave": 0.85,
  "rpm_wave": 0.42
}
```

---

## Home Assistant Auto-Discovery

The following entities are registered automatically when the first vital-sign frame arrives:

| Entity | Unit |
|---|---|
| Heart Rate | BPM |
| Breathing Rate | /min |
| Heart Rate Category | – |
| Breathing Category | – |
| Radar Status | – |

Zigbee device entities must be added manually in HA using the MQTT topics above, or you can use the existing MQTT integration with manual sensor definitions.

---

## Web UI

Open `http://<device-ip>/` in a browser.

![Web UI sections]

| Section | Description |
|---|---|
| Vital Values | Live heart rate, breathing rate and radar/MQTT connection status |
| Zigbee Devices | Table of all paired devices with editable friendly names |
| Permit Join | Open (180 s) or close the Zigbee network for new devices |

### REST API

```bash
# List all paired Zigbee devices
curl http://192.168.1.42/api/devices

# Get current vital sign readings
curl http://192.168.1.42/api/vitals

# Rename a device (addr in hex, name URL-encoded)
curl -X POST http://192.168.1.42/api/device \
  -d "addr=0x1234&name=Living+Room&idx=0"
```

---

## Supported Zigbee Clusters

| ZCL Cluster | Cluster ID | MQTT subtopic |
|---|---|---|
| On/Off | 0x0006 | `on_off` |
| Temperature Measurement | 0x0402 | `temperature` |
| Relative Humidity | 0x0405 | `humidity` |
| Illuminance Measurement | 0x0400 | `illuminance` |
| Occupancy Sensing | 0x0406 | `occupancy` |
| Any other | – | `raw` |

---

## Project Structure

```
zigbee-vital-sensor/
├── CMakeLists.txt              # IDF project, target esp32c6
├── sdkconfig.defaults          # Zigbee coordinator + HTTPD config
└── main/
    ├── idf_component.yml       # Zigbee SDK dependencies
    ├── main.c                  # app_main: WiFi → MQTT → radar → Zigbee → web
    ├── mr60bha1.h / .c         # MR60BHA1 UART driver (FreeRTOS task)
    ├── ha_mqtt.h / .c          # MQTT client + HA auto-discovery
    ├── zb_gateway.h / .c       # Zigbee coordinator, ZCL dispatch
    └── web_server.h / .c       # HTTP server on port 80
```

---

## License

MIT
