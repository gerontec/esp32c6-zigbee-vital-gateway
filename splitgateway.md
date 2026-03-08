# Split-Gateway-Design: ESP32-C6 minimal + Linux-Host intelligent

## Architektur-Überblick

```
┌─────────────────────────────────┐       MQTT        ┌────────────────────────────────┐
│        ESP32-C6 (C6)            │ ─────────────────▶ │    Linux-Host (Pi / Server)    │
│        Radio Bridge             │                    │    gateway_service.py          │
│                                 │ ◀───────────────── │                                │
│  ┌──────────────────────────┐   │  gw/<mac>/cmd/+   │  ✓ HA Auto-Discovery           │
│  │ Zigbee Coordinator       │   │                    │  ✓ Gerätename-Persistenz       │
│  │ (802.15.4 Radio)         │   │                    │  ✓ Web-Dashboard Port 8080     │
│  └──────────────────────────┘   │                    │  ✓ REST-API für Steuerung      │
│  ┌──────────────────────────┐   │                    │  ✓ JSON-Logging                │
│  │ MR60BHA1 UART-Driver     │   │                    └────────────────────────────────┘
│  └──────────────────────────┘   │                              │
│  ┌──────────────────────────┐   │                              │ MQTT
│  │ MQTT Bridge (kein HA)    │   │                              ▼
│  └──────────────────────────┘   │                    ┌────────────────────────────────┐
└─────────────────────────────────┘                    │     Mosquitto Broker           │
                                                        │     + Home Assistant           │
```

---

## Schicht 1 – ESP32-C6 (Radio Bridge, so dünn wie möglich)

### Was der C6 macht
| Aufgabe | Datei |
|---------|-------|
| Zigbee Coordinator (ZCL dispatch → raw MQTT) | `zb_gateway.c` |
| MR60BHA1 UART → raw MQTT | `mr60bha1.c` + `ha_mqtt.c` |
| MQTT publish/subscribe | `ha_mqtt.c` |
| Permit-Join via MQTT-Kommando | `main.c` + `ha_mqtt.c` |
| Permit-Join via Boot-Taste (Fallback) | `main.c` |

### Was der C6 NICHT mehr macht
- ❌ HA Auto-Discovery
- ❌ HTTP Webserver
- ❌ Gerätename-Datenbank
- ❌ String-Kategorien (nur noch rohe Integers)
- ❌ Irgendeine Logik über die Sensordaten hinaus

---

## MQTT-Topic-Schema

### Pub (C6 → Host)

| Topic | Payload | Beschreibung |
|-------|---------|--------------|
| `gw/<mac>/status` | `"online"` / `"offline"` | LWT + Connect-Announce |
| `gw/<mac>/mr60bha1` | `{"bpm":72,"rpm":16,"bpm_cat":1,"rpm_cat":1,"status":2,"bpm_wave":0.85,"rpm_wave":0.42}` | Vitaldaten (rohe Integers) |
| `gw/<mac>/zigbee/0x1234/on_off` | `{"v":1}` | 1=AN, 0=AUS |
| `gw/<mac>/zigbee/0x1234/temperature` | `{"raw":2150}` | 1/100 °C → Host dividiert |
| `gw/<mac>/zigbee/0x1234/humidity` | `{"raw":5500}` | 1/100 % → Host dividiert |
| `gw/<mac>/zigbee/0x1234/illuminance` | `{"raw":320}` | Lux |
| `gw/<mac>/zigbee/0x1234/occupancy` | `{"occ":1}` | 1=belegt, 0=frei |
| `gw/<mac>/zigbee/0x1234/join` | `{"event":"joined","addr":"0x1234","ieee":"01:02:03:04:05:06:07:08"}` | Gerät beigetreten |
| `gw/<mac>/zigbee/0x1234/raw` | `{"cluster":"0x0300","attr":"0x0000"}` | Unbekannte Cluster |
| `gw/<mac>/permit_join` | `{"open":true,"seconds":180}` | Pairing-Fenster-Status |

### Sub (Host → C6)

| Topic | Payload | Beschreibung |
|-------|---------|--------------|
| `gw/<mac>/cmd/permit_join` | `"180"` oder `"0"` | Join-Fenster öffnen/schließen |

### Kategorien-Mapping (bpm_cat / rpm_cat)
| Wert | Bedeutung |
|------|-----------|
| 0 | none (kein Signal) |
| 1 | normal |
| 2 | fast (zu schnell) |
| 3 | slow (zu langsam) |

### Status-Mapping (status)
| Wert | Bedeutung |
|------|-----------|
| 0 | init (Initialisierung) |
| 1 | calibrating (Kalibrierung) |
| 2 | measuring (Messung aktiv) |

---

## Schicht 2 – Linux-Host (gateway_service.py)

### Funktionen
- **MQTT-Subscriber**: Hört auf `gw/+/#` (alle Bridge-Instanzen)
- **HA Auto-Discovery**: Sendet `homeassistant/sensor/.../config` automatisch
  - Beim ersten `online`-Status → MR60BHA1-Entities
  - Beim ersten Paket je Zigbee-Cluster → Zigbee-Entity
- **Gerätename-Persistenz**: `/etc/esp32-gw/devices.json`
- **Web-Dashboard**: Port 8080, auto-refresh alle 10 s
- **REST-Endpunkte**:
  - `POST /api/permit_join` → sendet MQTT-Kommando an C6
  - `POST /api/rename` → speichert Gerätename
  - `GET  /api/state` → vollständiger State als JSON

### Konfiguration (Umgebungsvariablen)
| Variable | Standard | Beschreibung |
|----------|---------|--------------|
| `MQTT_HOST` | `localhost` | Mosquitto-Broker-IP |
| `MQTT_PORT` | `1883` | Mosquitto-Port |
| `MQTT_USER` | leer | MQTT-Benutzername (optional) |
| `MQTT_PASS` | leer | MQTT-Passwort (optional) |
| `WEB_PORT` | `8080` | Web-Dashboard-Port |
| `DEVICES_FILE` | `/etc/esp32-gw/devices.json` | Gerätename-Persistenz |
| `LOG_LEVEL` | `INFO` | Logging-Level |

---

## Installation auf dem Linux-Host (Raspberry Pi)

### Einmalig (passwortlos via SSH-Key)
```bash
# Von der Entwicklungsmaschine:
ssh-copy-id pi@192.168.178.218

# Dann:
scp -r host/ pi@192.168.178.218:~/esp32-gw/
ssh pi@192.168.178.218 'cd ~/esp32-gw && bash install.sh'
```

### Was install.sh macht
1. `mosquitto` + `python3-venv` installieren
2. Mosquitto auf Port 1883 (alle Interfaces, anonym) konfigurieren
3. `/opt/esp32-gw/` anlegen, Python-venv erstellen, paho-mqtt installieren
4. `esp32gw.service` in systemd einbinden und starten

### Service-Management
```bash
# Status prüfen
systemctl status esp32gw
journalctl -fu esp32gw

# Neu starten
systemctl restart esp32gw

# Permit Join manuell (ohne Web-UI)
mosquitto_pub -t gw/<mac>/cmd/permit_join -m 180
mosquitto_pub -t gw/<mac>/cmd/permit_join -m 0

# Alle MQTT-Nachrichten mitschneiden
mosquitto_sub -t 'gw/#' -v
```

---

## ESP32-C6 Firmware konfigurieren

In `main/main.c` anpassen:
```c
#define WIFI_SSID        "DeinNetz"
#define WIFI_PASSWORD    "DeinPasswort"
#define MQTT_BROKER_URI  "mqtt://192.168.178.218"  // IP des Pi
```

Bauen & Flashen:
```bash
cd zigbee-vital-sensor
idf.py build flash monitor
```

Das Base-Topic wird automatisch aus der MAC-Adresse abgeleitet:
```
gw/a1b2c3d4/...
```

---

## Robustheit

| Szenario | Verhalten |
|---------|-----------|
| MQTT-Broker nicht erreichbar | ESP32: reconnect automatisch; Host: reconnect automatisch |
| Host-Service crasht | systemd: RestartSec=5, automatischer Neustart |
| C6 verliert WiFi | reconnect + MQTT LWT publiziert `offline` |
| C6 Neustart | Zigbee-Netzwerk wird aus NVS wiederhergestellt |
| Host-Neustart | HA-Discovery wird beim nächsten `online`-Event neu gesendet |
| Permit Join ohne Host | Boot-Taste (GPIO9) öffnet direkt 180 s |
| Unbekannte Zigbee-Cluster | Werden als `raw`-Topic publiziert, kein Crash |

---

## Dateistruktur

```
esp-dev/
├── zigbee-vital-sensor/          ESP32-C6 Firmware (minimal)
│   └── main/
│       ├── main.c                WiFi + MQTT-Init + btn_task
│       ├── ha_mqtt.c/h           MQTT-Bridge (kein HA-Wissen)
│       ├── zb_gateway.c/h        Zigbee Coordinator (kein Naming)
│       └── mr60bha1.c/h          MR60BHA1 UART-Treiber (unverändert)
│
└── host/                         Linux-Host
    ├── gateway_service.py        Haupt-Service (MQTT + Web + HA-Discovery)
    ├── requirements.txt          paho-mqtt
    ├── esp32gw.service           systemd Unit
    └── install.sh                Installations-Script
```
