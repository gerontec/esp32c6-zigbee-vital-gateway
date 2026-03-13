# OTA-Flash ESP32C6 Zigbee-Firmware

## Voraussetzungen

- ESP32C6 im gleichen Netzwerk erreichbar (IPv4 bekannt)
- `esphome` Python-Paket installiert (`pip install esphome`)
- Firmware-Binary vorhanden: `firmware/zigbee-vital-sensor.bin`
- ESP32C6 läuft aktuell **ESPHome-Firmware** (Port 3232 offen)

## Schritt 1: Offene Ports prüfen

```bash
nmap -p 3232,80,8080,8266 <IP>
```

Port 3232 (ESPHome OTA) muss `open` sein.

## Schritt 2: OTA-Protokoll testen

```bash
python3 -c "
import socket
MAGIC = [0x6C, 0x26, 0xF7, 0x5C, 0x45]
s = socket.socket(); s.settimeout(5)
s.connect(('<IP>', 3232))
s.sendall(bytes(MAGIC))
print([hex(b) for b in s.recv(10)])
s.close()
"
```

Erwartete Antwort: `['0x0', '0x2']` → RESPONSE_OK, OTA-Version 2.0

## Schritt 3: Firmware flashen (OTA via espota2)

```bash
python3 -c "
import socket, logging, sys
from pathlib import Path
logging.basicConfig(level=logging.INFO, stream=sys.stderr)
from esphome.espota2 import perform_ota

bin_path = Path('firmware/zigbee-vital-sensor.bin')
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(120)
sock.connect(('<IP>', 3232))
with open(bin_path, 'rb') as f:
    perform_ota(sock, None, f, bin_path)
sock.close()
"
```

Dauer: ca. 8 Sekunden für ~1,47 MB.

## Schritt 4: MQTT-Broker-Adresse anpassen

Datei: `main/main.c`, Zeile 21:

```c
#define MQTT_BROKER_URI  "mqtt://192.168.178.218"
```

## Schritt 5: Firmware neu bauen

```bash
cd /home/pi/esp32c6-repo
source /home/pi/esp-idf/export.sh
idf.py build
cp build/zigbee-vital-sensor.bin firmware/zigbee-vital-sensor.bin
```

## Schritt 6: Neu gebaute Firmware flashen

Schritt 3 wiederholen mit der aktualisierten Binary.

## Wichtiger Hinweis

Nach dem ersten Flash auf die **Zigbee-Firmware** ist Port 3232 (ESPHome OTA)
**nicht mehr verfügbar** — die Zigbee-Firmware implementiert kein ESPHome-OTA.
Weiterer OTA-Flash nur noch möglich wenn:
- Die Zigbee-Firmware ein eigenes OTA implementiert, oder
- Das Gerät per **USB/Seriell** neu geflasht wird:

```bash
python3 -m esptool --chip esp32c6 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 2MB --flash_freq 80m \
  0x0  build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/zigbee-vital-sensor.bin
```

## WiFi-Credentials

In `main/main.c` vor dem Build eintragen (aktuell Platzhalter):

```c
#define WIFI_SSID        "DeinNetzwerk"
#define WIFI_PASSWORD    "DeinPasswort"
```

Das Pre-Built-Binary in `firmware/` hatte die echten Credentials kompiliert.
