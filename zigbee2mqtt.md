# ESP32-C6 Zigbee → MQTT Gateway

## Übersicht

Der Gateway verbindet einen **ESP32-C6** (Zigbee-Koordinator) über USB-Serial mit dem MQTT-Broker. Der ESP32-C6 sendet JSON-Lines über UART, der Gateway parsed diese und published sie als MQTT-Topics.

```
Zigbee-Geräte → ESP32-C6 → /dev/ttyACM1 → serial_gateway.py → MQTT (192.168.178.218:1883)
```

---

## Dateien

| Pfad | Beschreibung |
|------|-------------|
| `/home/pi/python/esp32c6-zigbee-vital-gateway/host/serial_gateway.py` | Haupt-Gateway-Skript |
| `/etc/systemd/system/zigbee-mqtt-gateway.service` | systemd-Service |

---

## systemd-Service

### Steuerbefehle

```bash
sudo systemctl start zigbee-mqtt-gateway     # starten
sudo systemctl stop zigbee-mqtt-gateway      # stoppen
sudo systemctl restart zigbee-mqtt-gateway   # neu starten
sudo systemctl status zigbee-mqtt-gateway    # Status anzeigen
journalctl -u zigbee-mqtt-gateway -f         # Live-Log
journalctl -u zigbee-mqtt-gateway -n 100     # letzte 100 Zeilen
```

### Service-Konfiguration

```ini
User=pi  Group=dialout
Port:   /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_B0:A6:04:87:10:E0-if00
Broker: 192.168.178.218:1883
Base:   gw/coordinator
Restart: on-failure, nach 5 s
```

Der stabile `by-id`-Pfad stellt sicher, dass der korrekte ESP32-C6 auch nach USB-Reconnect gebunden wird (MAC `B0:A6:04:87:10:E0`).

---

## Serial-Protokoll (ESP32-C6 → Pi)

Der ESP32-C6 sendet JSON-Lines über UART1 (`115200 8N1`). DTR/RTS bleiben LOW, damit kein ungewollter Reset ausgelöst wird.

### Nachrichtentypen

| `"t"` | Bedeutung | Beispiel |
|-------|-----------|---------|
| `boot` | ESP32-C6 gestartet | `{"t":"boot"}` |
| `heartbeat` | Lebenszeichen, alle ~30 s | `{"t":"heartbeat","uptime":120,"ch":15,"pan":4660}` |
| `vitals` | MR60BHA2-Vitaldaten | `{"t":"vitals","p":{"hr":72,"rr":16,...}}` |
| `zigbee` | Zigbee-Gerätedaten | `{"t":"zigbee","addr":4919,"sub":"temp","p":{"temp":21.5}}` |
| `permit_join` | Pairing-Fenster Status | `{"t":"permit_join","p":{"open":true,"seconds":180}}` |

---

## MQTT-Topics

### Vom Gateway published (ESP32-C6 → Broker)

| Topic | Inhalt | Retain | QoS |
|-------|--------|--------|-----|
| `gw/coordinator/status` | `online` / `offline` | ja | 1 |
| `gw/coordinator/console` | Rohe Serial-Ausgabe (jede Zeile) | nein | 0 |
| `gw/coordinator/heartbeat` | Heartbeat-JSON | ja | 1 |
| `gw/coordinator/mr60bha2` | Vitaldaten-JSON | nein | 0 |
| `gw/coordinator/zigbee/0x{addr}/{sub}` | Zigbee-Gerätedaten | nein | 0 |
| `gw/coordinator/permit_join` | Pairing-Status JSON | nein | 1 |

**Last Will:** `gw/coordinator/status` → `"offline"` (retain, QoS 1) — wird automatisch beim Verbindungsabbruch gesetzt.

### Kommandos (Broker → ESP32-C6)

Der Gateway abonniert `gw/coordinator/cmd/+` und leitet Kommandos über Serial an den ESP32-C6 weiter.

| Topic | Payload | Beschreibung |
|-------|---------|-------------|
| `gw/coordinator/cmd/permit_join` | Sekunden (z. B. `180`) | Pairing-Fenster öffnen, `0` = schließen |
| `gw/coordinator/cmd/set_channel` | Kanal `11`–`26` | Zigbee-Kanal wechseln |

**Beispiel – Pairing öffnen:**
```bash
mosquitto_pub -h 192.168.178.218 -t gw/coordinator/cmd/permit_join -m 180
```

---

## Skript-Parameter

```
serial_gateway.py [--port PORT] [--baud BAUD] [--broker HOST]
                  [--mqport PORT] [--user USER] [--pass PASS]
                  [--base BASE_TOPIC]
```

| Parameter | Standard | Beschreibung |
|-----------|----------|-------------|
| `--port` | `/dev/ttyACM0` | Serieller Port |
| `--baud` | `115200` | Baudrate |
| `--broker` | `192.168.178.218` | MQTT-Broker-Host |
| `--mqport` | `1883` | MQTT-Port |
| `--user` | – | MQTT-Benutzername (optional) |
| `--pass` | – | MQTT-Passwort (optional) |
| `--base` | `gw/c6serial` | MQTT-Basis-Topic |

---

## Hardware

| Komponente | Details |
|-----------|---------|
| Mikrocontroller | ESP32-C6 |
| USB-ID | `Espressif USB JTAG/serial debug unit` |
| MAC | `B0:A6:04:87:10:E0` |
| Gerät | `/dev/ttyACM1` (via USB-JTAG) |
| Sensor | Seeed MR60BHA2 (Atem- & Herzfrequenz) |
| Protokoll | Zigbee (Koordinator-Modus) |

---

## Troubleshooting

**Service startet nicht:**
```bash
journalctl -u zigbee-mqtt-gateway -n 50
ls -l /dev/serial/by-id/usb-Espressif*
```

**Port nicht gefunden** → USB-Kabel prüfen, `lsusb | grep Espressif`

**Kein MQTT** → `mosquitto_sub -h 192.168.178.218 -t 'gw/coordinator/#' -v`

**Pi-Benutzer nicht in dialout:**
```bash
sudo usermod -aG dialout pi
# danach neu anmelden oder Service neu starten
```
