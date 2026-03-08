# ESP32-C6 Matter over Thread – Vital Gateway

Zweite Firmware-Variante für denselben ESP32-C6 + MR60BHA1 Hardware-Stack.
Statt Zigbee 3.0 + MQTT wird hier **Matter over Thread** verwendet.

## Architektur

```
MR60BHA1 ──UART──► ESP32-C6 [ Matter End Device ]
                        │
                   Thread-Mesh (IEEE 802.15.4)
                        │
               Thread Border Router (RPi / GL.iNet)
                        │
                       IPv6
                        │
              Home Assistant (Matter Controller)
```

## Unterschied zur Zigbee-Variante

| | Zigbee-Firmware (`main/`) | Matter-Firmware (`matter/`) |
|---|---|---|
| Stack | esp-zigbee-sdk | esp-matter + OpenThread |
| Transport | Zigbee Mesh | Thread Mesh (IPv6) |
| Host-Software | gateway_service.py + MQTT | Kein Host nötig (native Matter) |
| Commissioning | Permit Join | BLE GATT (QR-Code) |
| HA-Integration | MQTT Auto-Discovery | Native Matter |

## Matter Data Model

| Endpoint | Cluster | Inhalt |
|---|---|---|
| 0 | Basic Information | Device info, OTA |
| 1 | Occupancy Sensing | Radar-Präsenz (radar_status == 2) |
| 2 | (Custom 0xFFF10001) | Herzrate BPM + Kategorie |
| 3 | (Custom 0xFFF10002) | Atemrate RPM + Kategorie |

## Voraussetzungen

```bash
# ESP-IDF >= 5.1
. $IDF_PATH/export.sh

# ESP-Matter SDK
git clone --recursive https://github.com/espressif/esp-matter $ESP_MATTER_PATH
. $ESP_MATTER_PATH/export.sh
```

## Bauen & Flashen

```bash
cd matter/
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Matter Commissioning

Nach dem Flash mit Apple Home, Google Home oder Home Assistant:
1. QR-Code aus `idf.py monitor` ablesen
2. Im Controller „Gerät hinzufügen" → QR-Code scannen
3. Gerät erscheint als „Vital Sensor" ohne weiteren Gateway

## Status

- [x] Projektgerüst
- [x] MR60BHA1 UART-Treiber (wiederverwendet)
- [x] Matter Occupancy-Endpoint
- [ ] Custom Cluster BPM/RPM
- [ ] OTA-Update via Matter
- [ ] Thread Border Router Anleitung
