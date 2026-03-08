#!/usr/bin/env python3
"""
ESP32-C6 Radio Bridge – Linux Host Gateway Service
===================================================
Schicht 2: Alles Intelligente läuft hier.

Funktionen:
  - Subscribt auf gw/+/# (alle Bridge-Instanzen)
  - HA Auto-Discovery für MR60BHA1 und Zigbee-Geräte
  - Gerätename-Persistenz in devices.json
  - Web-Dashboard auf Port 8080
  - Permit-Join-Kommandos via REST → MQTT

Topics vom ESP32-C6 (rein):
  gw/<mac>/status                  → "online"/"offline"
  gw/<mac>/mr60bha1                → {"bpm":72,"rpm":16,"bpm_cat":1,...}
  gw/<mac>/zigbee/0x1234/on_off    → {"v":1}
  gw/<mac>/zigbee/0x1234/temperature → {"raw":2150}
  gw/<mac>/zigbee/0x1234/humidity  → {"raw":5500}
  gw/<mac>/zigbee/0x1234/illuminance → {"raw":320}
  gw/<mac>/zigbee/0x1234/occupancy → {"occ":1}
  gw/<mac>/zigbee/0x1234/join      → {"event":"joined","addr":"0x1234","ieee":"..."}
  gw/<mac>/permit_join             → {"open":true,"seconds":180}

Kommandos an ESP32-C6 (raus):
  gw/<mac>/cmd/permit_join         → "180" (Sekunden)
"""

import json
import logging
import os
import re
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import paho.mqtt.client as mqtt
import pymysql
import pymysql.cursors

# ── Konfiguration ────────────────────────────────────────────────────────────
MQTT_HOST      = os.getenv("MQTT_HOST",    "localhost")
MQTT_PORT      = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER      = os.getenv("MQTT_USER",    None)
MQTT_PASS      = os.getenv("MQTT_PASS",    None)
WEB_PORT       = int(os.getenv("WEB_PORT", "8080"))
DEVICES_FILE   = Path(os.getenv("DEVICES_FILE", "/etc/esp32-gw/devices.json"))
LOG_LEVEL      = os.getenv("LOG_LEVEL", "INFO")

DB_HOST        = os.getenv("DB_HOST",  "192.168.178.218")
DB_USER        = os.getenv("DB_USER",  "gh")
DB_PASS        = os.getenv("DB_PASS",  "a12345")
DB_NAME        = os.getenv("DB_NAME",  "wagodb")
# ─────────────────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=getattr(logging, LOG_LEVEL),
    format="%(asctime)s %(levelname)-7s %(name)s – %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("gw")

# ── MariaDB ───────────────────────────────────────────────────────────────────
# Datenbankschicht – Verbindung: -h 192.168.178.218 -u gh -pa12345 wagodb
#
# Tabellen:
#
#   esp32_gateways          – Ein Eintrag je Gateway-MAC (UPSERT)
#     mac        CHAR(8)      Eindeutige Gateway-ID (z.B. "aabbccdd")
#     status     VARCHAR(16)  "online" / "offline"
#     last_seen  DATETIME     Letzte Aktivität
#
#   esp32_vitals             – Zeitreihe MR60BHA1-Messwerte (INSERT)
#     id         INT PK AUTO
#     mac        CHAR(8)      Zugehöriger Gateway
#     ts         DATETIME(3)  Messzeitpunkt (ms-Auflösung)
#     bpm        SMALLINT     Herzrate
#     rpm        SMALLINT     Atemrate
#     bpm_cat    TINYINT      0=none 1=normal 2=fast 3=slow
#     rpm_cat    TINYINT      0=none 1=normal 2=fast 3=slow
#     radar_status TINYINT    0=init 1=calibrating 2=measuring
#
#   esp32_zigbee_devices     – Ein Eintrag je Zigbee-Gerät (UPSERT)
#     mac        CHAR(8)      Zugehöriger Gateway
#     addr       VARCHAR(8)   Zigbee-Kurzadresse (z.B. "0x1a2b")
#     ieee       VARCHAR(24)  IEEE-Langadresse
#     name       VARCHAR(64)  Freundlicher Name (aus devices.json)
#     last_seen  DATETIME     Letztes Datenpaket
#
#   esp32_zigbee_data        – Zeitreihe Zigbee-Sensorwerte (INSERT)
#     id         INT PK AUTO
#     mac        CHAR(8)      Zugehöriger Gateway
#     addr       VARCHAR(8)   Zigbee-Kurzadresse
#     cluster    VARCHAR(32)  Cluster-Name: temperature/humidity/illuminance/on_off/occupancy
#     ts         DATETIME(3)  Messzeitpunkt (ms-Auflösung)
#     value      DOUBLE       Skalierter Hauptwert (°C, %, lx, 0/1)
#     raw_json   TEXT         Original-JSON-Payload
#
# Reconnect: _db_cursor() ruft ping(reconnect=True) vor jedem Statement.
# Thread-Safety: Alle DB-Operationen laufen unter _db_lock.
# ─────────────────────────────────────────────────────────────────────────────
_db_lock = threading.Lock()
_db_conn = None


def _db_connect():
    return pymysql.connect(
        host=DB_HOST, user=DB_USER, password=DB_PASS, database=DB_NAME,
        charset="utf8mb4", autocommit=True,
        connect_timeout=10,
    )


def _db_init():
    """Tabellen anlegen falls nicht vorhanden."""
    global _db_conn
    _db_conn = _db_connect()
    cur = _db_conn.cursor()

    cur.execute("""
        CREATE TABLE IF NOT EXISTS esp32_gateways (
            mac        CHAR(8)      NOT NULL,
            status     VARCHAR(16)  NOT NULL DEFAULT 'offline',
            last_seen  DATETIME,
            PRIMARY KEY (mac)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS esp32_vitals (
            id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
            mac        CHAR(8)      NOT NULL,
            ts         DATETIME(3)  NOT NULL,
            bpm        SMALLINT UNSIGNED,
            rpm        SMALLINT UNSIGNED,
            bpm_cat    TINYINT UNSIGNED,
            rpm_cat    TINYINT UNSIGNED,
            radar_status TINYINT UNSIGNED,
            INDEX (mac, ts)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS esp32_zigbee_devices (
            mac        CHAR(8)      NOT NULL,
            addr       VARCHAR(8)   NOT NULL,
            ieee       VARCHAR(24)  NOT NULL DEFAULT '',
            name       VARCHAR(64)  NOT NULL DEFAULT '',
            last_seen  DATETIME,
            PRIMARY KEY (mac, addr)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS esp32_zigbee_data (
            id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
            mac        CHAR(8)      NOT NULL,
            addr       VARCHAR(8)   NOT NULL,
            cluster    VARCHAR(32)  NOT NULL,
            ts         DATETIME(3)  NOT NULL,
            value      DOUBLE,
            raw_json   TEXT,
            INDEX (mac, addr, cluster, ts)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    """)

    cur.close()
    log.info("DB initialisiert auf %s/%s", DB_HOST, DB_NAME)


def _db_cursor():
    """Gibt einen Cursor zurück, reconnectet bei Bedarf."""
    global _db_conn
    try:
        _db_conn.ping(reconnect=True)
    except Exception:
        _db_conn = _db_connect()
    return _db_conn.cursor()


def db_upsert_gateway(mac, status):
    with _db_lock:
        try:
            cur = _db_cursor()
            cur.execute("""
                INSERT INTO esp32_gateways (mac, status, last_seen)
                VALUES (%s, %s, NOW())
                ON DUPLICATE KEY UPDATE status=VALUES(status), last_seen=NOW()
            """, (mac, status))
            cur.close()
        except Exception as e:
            log.warning("DB gateway upsert: %s", e)


def db_insert_vitals(mac, data):
    with _db_lock:
        try:
            cur = _db_cursor()
            cur.execute("""
                INSERT INTO esp32_vitals
                    (mac, ts, bpm, rpm, bpm_cat, rpm_cat, radar_status)
                VALUES (%s, NOW(3), %s, %s, %s, %s, %s)
            """, (mac,
                  data.get("bpm"), data.get("rpm"),
                  data.get("bpm_cat"), data.get("rpm_cat"),
                  data.get("status")))
            cur.close()
        except Exception as e:
            log.warning("DB vitals insert: %s", e)


def db_upsert_zigbee_device(mac, addr, ieee, name):
    with _db_lock:
        try:
            cur = _db_cursor()
            cur.execute("""
                INSERT INTO esp32_zigbee_devices (mac, addr, ieee, name, last_seen)
                VALUES (%s, %s, %s, %s, NOW())
                ON DUPLICATE KEY UPDATE ieee=VALUES(ieee), name=VALUES(name), last_seen=NOW()
            """, (mac, addr, ieee, name))
            cur.close()
        except Exception as e:
            log.warning("DB zigbee device upsert: %s", e)


def db_insert_zigbee_data(mac, addr, cluster, raw, value):
    with _db_lock:
        try:
            cur = _db_cursor()
            cur.execute("""
                INSERT INTO esp32_zigbee_data
                    (mac, addr, cluster, ts, value, raw_json)
                VALUES (%s, %s, %s, NOW(3), %s, %s)
            """, (mac, addr, cluster, value, json.dumps(raw)))
            cur.close()
            # last_seen aktualisieren
            cur2 = _db_cursor()
            cur2.execute("""
                UPDATE esp32_zigbee_devices SET last_seen=NOW()
                WHERE mac=%s AND addr=%s
            """, (mac, addr))
            cur2.close()
        except Exception as e:
            log.warning("DB zigbee data insert: %s", e)


def _extract_value(cluster, raw):
    """Numerischen Hauptwert aus raw-Payload extrahieren."""
    if cluster == "temperature":
        return raw.get("raw", 0) / 100.0
    if cluster == "humidity":
        return raw.get("raw", 0) / 100.0
    if cluster == "illuminance":
        return raw.get("raw")
    if cluster == "on_off":
        return raw.get("v")
    if cluster == "occupancy":
        return raw.get("occ")
    return None


# ── Kategorien-Mapping (bpm_cat / rpm_cat: 0-3) ──────────────────────────────
CAT = {0: "none", 1: "normal", 2: "fast", 3: "slow"}
STATUS_MAP = {0: "init", 1: "calibrating", 2: "measuring"}

# ── Globaler State ────────────────────────────────────────────────────────────
_lock     = threading.Lock()
_gateways = {}   # mac → {status, vitals, devices, permit_join}
_names    = {}   # "mac/0xaddr" → friendly_name
_client   = None


def _load_names():
    global _names
    try:
        if DEVICES_FILE.exists():
            _names = json.loads(DEVICES_FILE.read_text())
            log.info("Gerätenamen geladen: %d Einträge", len(_names))
    except Exception as e:
        log.warning("Gerätenamen konnten nicht geladen werden: %s", e)


def _save_names():
    try:
        DEVICES_FILE.parent.mkdir(parents=True, exist_ok=True)
        DEVICES_FILE.write_text(json.dumps(_names, indent=2))
    except Exception as e:
        log.warning("Gerätenamen konnten nicht gespeichert werden: %s", e)


def _gw(mac):
    """Gateway-Eintrag holen oder anlegen."""
    if mac not in _gateways:
        _gateways[mac] = {
            "status":     "offline",
            "vitals":     None,
            "devices":    {},   # addr → {ieee, last_seen, last_payload}
            "permit_join": {"open": False, "seconds": 0},
            "last_seen":  None,
        }
    return _gateways[mac]


# ── HA Auto-Discovery ─────────────────────────────────────────────────────────
def _publish_mr60_discovery(mac):
    base = f"gw/{mac}"
    state_topic = f"{base}/mr60bha1"
    avail_topic = f"{base}/status"
    dev_payload = {
        "identifiers": [f"esp32c6_{mac}"],
        "name": f"ESP32-C6 Bridge {mac}",
        "model": "ESP32-C6 + MR60BHA1",
        "manufacturer": "Gerontec",
    }

    sensors = [
        ("bpm",    "Heart Rate",        "{{ value_json.bpm }}",     "BPM",  "mdi:heart-pulse",    None),
        ("rpm",    "Breathing Rate",    "{{ value_json.rpm }}",     "/min", "mdi:lungs",           None),
        ("bpm_cat","HR Category",
            "{% set m={0:'none',1:'normal',2:'fast',3:'slow'} %}"
            "{{ m[value_json.bpm_cat] | default('unknown') }}",
            "", "mdi:heart", None),
        ("rpm_cat","Breath Category",
            "{% set m={0:'none',1:'normal',2:'fast',3:'slow'} %}"
            "{{ m[value_json.rpm_cat] | default('unknown') }}",
            "", "mdi:weather-windy", None),
        ("radar_status","Radar Status",
            "{% set m={0:'init',1:'calibrating',2:'measuring'} %}"
            "{{ m[value_json.status] | default('unknown') }}",
            "", "mdi:radar", None),
    ]

    for obj_id, name, tpl, unit, icon, dev_class in sensors:
        topic = f"homeassistant/sensor/{base}/{obj_id}/config"
        payload = {
            "name":                 name,
            "unique_id":            f"{mac}_{obj_id}",
            "state_topic":          state_topic,
            "value_template":       tpl,
            "unit_of_measurement":  unit,
            "icon":                 icon,
            "availability_topic":   avail_topic,
            "payload_available":    "online",
            "payload_not_available":"offline",
            "device":               dev_payload,
        }
        if dev_class:
            payload["device_class"] = dev_class
        _client.publish(topic, json.dumps(payload), qos=1, retain=True)

    log.info("[%s] HA-Discovery MR60BHA1 published", mac)


def _publish_zigbee_discovery(mac, addr, cluster):
    base = f"gw/{mac}"
    avail_topic = f"{base}/status"
    state_topic = f"{base}/zigbee/{addr}/{cluster}"
    dev_key = f"{mac}/{addr}"
    friendly = _names.get(dev_key, addr)

    dev_payload = {
        "identifiers": [f"zigbee_{mac}_{addr}"],
        "name": friendly,
        "via_device": f"esp32c6_{mac}",
    }

    cluster_map = {
        "on_off": [
            ("binary_sensor", "on_off_state", "State",
             "{{ 'ON' if value_json.v == 1 else 'OFF' }}", "", "mdi:toggle-switch", None)
        ],
        "temperature": [
            ("sensor", "temperature", "Temperature",
             "{{ (value_json.raw / 100) | round(1) }}", "°C", "mdi:thermometer", "temperature")
        ],
        "humidity": [
            ("sensor", "humidity", "Humidity",
             "{{ (value_json.raw / 100) | round(1) }}", "%", "mdi:water-percent", "humidity")
        ],
        "illuminance": [
            ("sensor", "illuminance", "Illuminance",
             "{{ value_json.raw }}", "lx", "mdi:brightness-5", "illuminance")
        ],
        "occupancy": [
            ("binary_sensor", "occupancy", "Occupancy",
             "{{ 'ON' if value_json.occ == 1 else 'OFF' }}", "", "mdi:motion-sensor", "occupancy")
        ],
    }

    if cluster not in cluster_map:
        return

    for comp, obj_id, name, tpl, unit, icon, dev_class in cluster_map[cluster]:
        uid = f"zigbee_{mac}_{addr}_{obj_id}"
        topic = f"homeassistant/{comp}/{uid}/config"
        payload = {
            "name":                 f"{friendly} {name}",
            "unique_id":            uid,
            "state_topic":          state_topic,
            "value_template":       tpl,
            "availability_topic":   avail_topic,
            "payload_available":    "online",
            "payload_not_available":"offline",
            "device":               dev_payload,
            "icon":                 icon,
        }
        if unit:
            payload["unit_of_measurement"] = unit
        if dev_class:
            payload["device_class"] = dev_class
        _client.publish(topic, json.dumps(payload), qos=1, retain=True)


# ── MQTT Message Dispatcher ───────────────────────────────────────────────────
def _on_message(client, userdata, msg):
    topic = msg.topic
    try:
        raw = msg.payload.decode("utf-8", errors="replace")
    except Exception:
        return

    # gw/<mac>/<rest...>
    m = re.match(r"^gw/([0-9a-f]{8})/(.+)$", topic)
    if not m:
        return
    mac, rest = m.group(1), m.group(2)

    with _lock:
        gw = _gw(mac)
        gw["last_seen"] = datetime.utcnow().isoformat() + "Z"

        # ── Status ──────────────────────────────────────────────────────────
        if rest == "status":
            gw["status"] = raw.strip()
            log.info("[%s] status = %s", mac, raw.strip())
            db_upsert_gateway(mac, raw.strip())
            if raw.strip() == "online":
                _publish_mr60_discovery(mac)

        # ── MR60BHA1 Vitaldaten ──────────────────────────────────────────────
        elif rest == "mr60bha1":
            try:
                data = json.loads(raw)
                data["bpm_cat_str"]  = CAT.get(data.get("bpm_cat"), "?")
                data["rpm_cat_str"]  = CAT.get(data.get("rpm_cat"), "?")
                data["status_str"]   = STATUS_MAP.get(data.get("status"), "?")
                data["ts"] = datetime.utcnow().isoformat() + "Z"
                gw["vitals"] = data
                db_insert_vitals(mac, data)
                log.debug("[%s] vitals bpm=%d rpm=%d",
                          mac, data.get("bpm", -1), data.get("rpm", -1))
            except json.JSONDecodeError:
                log.warning("[%s] ungültiges vitals JSON: %s", mac, raw[:80])

        # ── Zigbee Gerät beigetreten ─────────────────────────────────────────
        elif re.match(r"^zigbee/(0x[0-9a-f]{4})/join$", rest):
            addr_m = re.match(r"^zigbee/(0x[0-9a-f]{4})/join$", rest)
            addr = addr_m.group(1)
            try:
                data = json.loads(raw)
                dev_key = f"{mac}/{addr}"
                if addr not in gw["devices"]:
                    gw["devices"][addr] = {}
                ieee = data.get("ieee", "?")
                name = _names.get(dev_key, addr)
                gw["devices"][addr].update({
                    "ieee":      ieee,
                    "last_seen": datetime.utcnow().isoformat() + "Z",
                    "name":      name,
                })
                db_upsert_zigbee_device(mac, addr, ieee, name)
                log.info("[%s] Zigbee join: %s ieee=%s", mac, addr, ieee)
            except json.JSONDecodeError:
                pass

        # ── Zigbee Sensor-Daten ──────────────────────────────────────────────
        elif re.match(r"^zigbee/(0x[0-9a-f]{4})/(\w+)$", rest):
            zm = re.match(r"^zigbee/(0x[0-9a-f]{4})/(\w+)$", rest)
            addr, cluster = zm.group(1), zm.group(2)
            if addr not in gw["devices"]:
                gw["devices"][addr] = {"name": _names.get(f"{mac}/{addr}", addr)}
            dev = gw["devices"][addr]
            dev["last_seen"] = datetime.utcnow().isoformat() + "Z"
            try:
                payload = json.loads(raw)
                dev.setdefault("clusters", {})[cluster] = {
                    "raw": payload,
                    "ts":  datetime.utcnow().isoformat() + "Z",
                }
                value = _extract_value(cluster, payload)
                db_insert_zigbee_data(mac, addr, cluster, payload, value)
                # HA-Discovery beim ersten Paket je Cluster
                disc_key = f"disc_{mac}_{addr}_{cluster}"
                if not userdata.get(disc_key):
                    userdata[disc_key] = True
                    _publish_zigbee_discovery(mac, addr, cluster)
            except json.JSONDecodeError:
                pass

        # ── Permit Join Status ───────────────────────────────────────────────
        elif rest == "permit_join":
            try:
                gw["permit_join"] = json.loads(raw)
                log.info("[%s] permit_join %s", mac, raw)
            except json.JSONDecodeError:
                pass


def _on_connect(client, userdata, flags, rc, props=None):
    if rc == 0:
        log.info("MQTT verbunden mit %s:%d", MQTT_HOST, MQTT_PORT)
        client.subscribe("gw/+/#", qos=1)
    else:
        log.error("MQTT Verbindungsfehler rc=%d", rc)


def _on_disconnect(client, userdata, rc, props=None):
    log.warning("MQTT getrennt (rc=%d) – verbinde erneut…", rc)


# ── Web Dashboard ─────────────────────────────────────────────────────────────
_DISCOVERY_CACHE: dict = {}   # für _on_message userdata


def _html_dashboard():
    """Minimales HTML-Dashboard (inline, kein externes Framework)."""
    rows_gw = ""
    with _lock:
        gateways_snapshot = json.loads(json.dumps(_gateways))

    for mac, gw in gateways_snapshot.items():
        vitals = gw.get("vitals") or {}
        pj = gw.get("permit_join", {})

        # Geräte-Tabelle
        dev_rows = ""
        for addr, dev in gw.get("devices", {}).items():
            dev_key = f"{mac}/{addr}"
            name = _names.get(dev_key, addr)
            clusters = dev.get("clusters", {})
            cl_str = ", ".join(
                f"{c}={list(v['raw'].values())[0]}"
                for c, v in clusters.items()
                if v.get("raw")
            )
            dev_rows += (
                f"<tr>"
                f"<td>{addr}</td>"
                f"<td><form method='POST' action='/api/rename'>"
                f"<input name='key' type='hidden' value='{dev_key}'>"
                f"<input name='name' value='{name}' size='18'>"
                f"<button>✎</button></form></td>"
                f"<td>{dev.get('ieee','?')}</td>"
                f"<td>{cl_str}</td>"
                f"<td>{dev.get('last_seen','?')[:19]}</td>"
                f"</tr>"
            )

        pj_open = "🔓 offen" if pj.get("open") else "🔒 geschlossen"
        rows_gw += f"""
        <section>
          <h2>Gateway <code>{mac}</code>
            <span class="badge {'online' if gw['status']=='online' else 'offline'}">{gw['status']}</span>
          </h2>
          <h3>MR60BHA1 Vital Signs</h3>
          <table>
            <tr><th>Herzrate</th><td>{vitals.get('bpm','–')} BPM ({vitals.get('bpm_cat_str','–')})</td></tr>
            <tr><th>Atemrate</th><td>{vitals.get('rpm','–')} /min ({vitals.get('rpm_cat_str','–')})</td></tr>
            <tr><th>Status</th><td>{vitals.get('status_str','–')}</td></tr>
            <tr><th>Letzte Messung</th><td>{vitals.get('ts','–')[:19] if vitals else '–'}</td></tr>
          </table>

          <h3>Permit Join: {pj_open}</h3>
          <form method='POST' action='/api/permit_join'>
            <input name='mac' type='hidden' value='{mac}'>
            <button name='secs' value='180'>🔓 180 s öffnen</button>
            <button name='secs' value='0'>🔒 Schließen</button>
          </form>

          <h3>Zigbee-Geräte</h3>
          <table>
            <tr><th>Adresse</th><th>Name</th><th>IEEE</th><th>Werte</th><th>Zuletzt</th></tr>
            {dev_rows or '<tr><td colspan=5><em>keine Geräte</em></td></tr>'}
          </table>
        </section>
        """

    return f"""<!DOCTYPE html>
<html lang="de">
<head><meta charset="utf-8">
<meta http-equiv="refresh" content="10">
<title>ESP32-C6 Gateway</title>
<style>
  body{{background:#1a1a2e;color:#e0e0e0;font-family:monospace;margin:2em}}
  h1{{color:#00d4ff}} h2{{color:#a0c4ff;border-bottom:1px solid #333;padding-bottom:.3em}}
  h3{{color:#7ecbff;margin-top:1.2em}}
  table{{border-collapse:collapse;width:100%;margin:.5em 0}}
  th,td{{border:1px solid #444;padding:.4em .8em;text-align:left}}
  th{{background:#2a2a4a}} tr:nth-child(even){{background:#1e1e3a}}
  .badge{{padding:.2em .6em;border-radius:4px;font-size:.85em;margin-left:.5em}}
  .online{{background:#1a5c1a;color:#7fff7f}}
  .offline{{background:#5c1a1a;color:#ff7f7f}}
  button{{background:#2a4a7a;color:#e0e0e0;border:1px solid #4a6a9a;
          padding:.3em .8em;cursor:pointer;border-radius:3px}}
  button:hover{{background:#3a6aaa}}
  input[type=text],input:not([type]){{background:#2a2a4a;color:#e0e0e0;
          border:1px solid #555;padding:.25em .4em}}
  section{{background:#16213e;padding:1.2em;margin:1.2em 0;border-radius:6px}}
  footer{{margin-top:2em;color:#666;font-size:.85em}}
</style>
</head>
<body>
<h1>ESP32-C6 Radio Bridge Dashboard</h1>
<p>Auto-Refresh alle 10 s &nbsp;|&nbsp; <a href="/api/state" style="color:#7ecbff">JSON API</a></p>
{rows_gw or '<p><em>Noch keine Gateway-Daten empfangen.</em></p>'}
<footer>
  <strong>MQTT-Kommandos:</strong><br>
  Permit Join öffnen: <code>mosquitto_pub -t gw/&lt;mac&gt;/cmd/permit_join -m 180</code><br>
  Permit Join schließen: <code>mosquitto_pub -t gw/&lt;mac&gt;/cmd/permit_join -m 0</code>
</footer>
</body></html>"""


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # kein Standard-Access-Log

    def _send(self, code, content_type, body):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", len(data))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/" or path == "/index.html":
            self._send(200, "text/html; charset=utf-8", _html_dashboard())
        elif path == "/api/state":
            with _lock:
                data = json.dumps(_gateways, indent=2, default=str)
            self._send(200, "application/json", data)
        else:
            self._send(404, "text/plain", "Not Found")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", errors="replace")
        params = parse_qs(body)

        path = urlparse(self.path).path

        if path == "/api/permit_join":
            mac  = params.get("mac",  [""])[0]
            secs = params.get("secs", ["0"])[0]
            if mac and _client:
                topic = f"gw/{mac}/cmd/permit_join"
                _client.publish(topic, secs, qos=1)
                log.info("permit_join %s s → %s", secs, topic)
            self.send_response(303)
            self.send_header("Location", "/")
            self.end_headers()

        elif path == "/api/rename":
            key  = params.get("key",  [""])[0]
            name = params.get("name", [""])[0].strip()
            if key and name:
                with _lock:
                    _names[key] = name
                _save_names()
                log.info("Umbenannt: %s → %s", key, name)
            self.send_response(303)
            self.send_header("Location", "/")
            self.end_headers()

        else:
            self._send(404, "text/plain", "Not Found")


def _run_web(port):
    srv = HTTPServer(("0.0.0.0", port), _Handler)
    log.info("Web-Dashboard auf http://0.0.0.0:%d/", port)
    srv.serve_forever()


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    _db_init()
    _load_names()

    global _client
    userdata = _DISCOVERY_CACHE
    _client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, userdata=userdata)
    if MQTT_USER:
        _client.username_pw_set(MQTT_USER, MQTT_PASS)
    _client.on_connect    = _on_connect
    _client.on_disconnect = _on_disconnect
    _client.on_message    = _on_message

    _client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)

    # Web-Server in Hintergrund-Thread
    t = threading.Thread(target=_run_web, args=(WEB_PORT,), daemon=True)
    t.start()

    log.info("Gateway Service gestartet – MQTT=%s:%d", MQTT_HOST, MQTT_PORT)
    _client.loop_forever()


if __name__ == "__main__":
    main()
