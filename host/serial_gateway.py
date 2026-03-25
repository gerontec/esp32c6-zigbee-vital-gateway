#!/usr/bin/env python3
"""
serial_gateway.py – liest JSON-Lines vom ESP32-C6 (COM-Port)
und published sie als MQTT an den Broker + schreibt in MariaDB.
Web-Dashboard auf Port 8080.

Verwendung:
    python3 serial_gateway.py
    python3 serial_gateway.py --port /dev/ttyACM1 --broker 192.168.178.218
"""
import argparse
import json
import os
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

import serial
import paho.mqtt.client as mqtt
import pymysql

# ── Konfiguration ─────────────────────────────────────────────────────────
DEFAULT_PORT   = "/dev/ttyACM0"
DEFAULT_BAUD   = 115200
DEFAULT_BROKER = "192.168.178.218"
DEFAULT_MQPORT = 1883
WEB_PORT       = int(os.getenv("WEB_PORT", "8080"))

DB_HOST = os.getenv("DB_HOST", "192.168.178.218")
DB_PORT = int(os.getenv("DB_PORT", "3306"))
DB_USER = os.getenv("DB_USER", "gh")
DB_PASS = os.getenv("DB_PASS", "a12345")
DB_NAME = os.getenv("DB_NAME", "wagodb")

# ── Argumente ──────────────────────────────────────────────────────────────
ap = argparse.ArgumentParser()
ap.add_argument("--port",   default=DEFAULT_PORT)
ap.add_argument("--baud",   default=DEFAULT_BAUD,   type=int)
ap.add_argument("--broker", default=DEFAULT_BROKER)
ap.add_argument("--mqport", default=DEFAULT_MQPORT, type=int)
ap.add_argument("--user",   default=None)
ap.add_argument("--pass",   default=None, dest="passwd")
ap.add_argument("--base",   default="gw/c6serial",
                help="MQTT base topic (z.B. gw/coordinator)")
args = ap.parse_args()

BASE   = args.base
DB_MAC = BASE.split("/")[-1][:8]

# ── In-Memory State ────────────────────────────────────────────────────────
_state_lock = threading.Lock()
_state = {
    "status":       "offline",
    "uptime":       0,
    "channel":      0,
    "pan":          "–",
    "permit_join":  {"open": False, "seconds": 0},
    "devices":      {},   # addr → {ieee, name, last_seen, clusters: {name: value}}
    "ota": {
        "status":      "idle",   # idle | pending | transferring | done | error
        "bin_path":    "",
        "size":        0,
        "version":     1,
        "transferred": 0,
    },
}

# ── OTA state machine ───────────────────────────────────────────────────────
_ota_file = None   # open file handle during transfer

def _now():
    return datetime.now().strftime("%d.%m. %H:%M:%S")

# ── MariaDB ────────────────────────────────────────────────────────────────
_db_conn = None
_db_lock = threading.Lock()

def _db_connect():
    return pymysql.connect(
        host=DB_HOST, port=DB_PORT,
        user=DB_USER, password=DB_PASS,
        database=DB_NAME,
        autocommit=True,
        connect_timeout=5,
    )

def _db_cursor():
    global _db_conn
    try:
        _db_conn.ping(reconnect=True)
    except Exception:
        _db_conn = _db_connect()
    return _db_conn.cursor()

def db_init():
    global _db_conn
    try:
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
            CREATE TABLE IF NOT EXISTS esp32_chan_scan (
                id      INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
                mac     CHAR(8)      NOT NULL,
                ts      DATETIME(3)  NOT NULL,
                ch      TINYINT UNSIGNED NOT NULL,
                count   TINYINT UNSIGNED NOT NULL DEFAULT 0,
                nets    TEXT,
                INDEX (mac, ts)
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
        # Bekannte Devices aus DB in State laden
        cur.execute("""
            SELECT addr, ieee, name, last_seen FROM esp32_zigbee_devices WHERE mac=%s
        """, (DB_MAC,))
        with _state_lock:
            for row in cur.fetchall():
                addr, ieee, name, last_seen = row
                ts = last_seen.strftime("%d.%m. %H:%M:%S") if last_seen else "–"
                _state["devices"].setdefault(addr, {
                    "ieee": ieee or "", "name": name or addr,
                    "last_seen": ts, "clusters": {}
                })
        cur.close()
        print(f"[DB] verbunden {DB_HOST}/{DB_NAME} mac={DB_MAC} devices={len(_state['devices'])}")
    except Exception as e:
        print(f"[DB] Fehler bei Init: {e}")
        _db_conn = None

def db_insert_chan_scan(ch, count, nets_json):
    with _db_lock:
        try:
            cur = _db_cursor()
            cur.execute("""
                INSERT INTO esp32_chan_scan (mac, ts, ch, count, nets)
                VALUES (%s, UTC_TIMESTAMP(3), %s, %s, %s)
            """, (DB_MAC, ch, count, nets_json))
            cur.close()
        except Exception as e:
            print(f"[DB] chan_scan insert: {e}")

def db_upsert_gateway(status):
    with _db_lock:
        try:
            cur = _db_cursor()
            cur.execute("""
                INSERT INTO esp32_gateways (mac, status, last_seen)
                VALUES (%s, %s, UTC_TIMESTAMP())
                ON DUPLICATE KEY UPDATE status=VALUES(status), last_seen=UTC_TIMESTAMP()
            """, (DB_MAC, status[:16]))
            cur.close()
        except Exception as e:
            print(f"[DB] gateway upsert: {e}")

def db_upsert_zigbee_device(addr, ieee=""):
    with _db_lock:
        try:
            cur = _db_cursor()
            cur.execute("""
                INSERT INTO esp32_zigbee_devices (mac, addr, ieee, last_seen)
                VALUES (%s, %s, %s, UTC_TIMESTAMP())
                ON DUPLICATE KEY UPDATE ieee=VALUES(ieee), last_seen=UTC_TIMESTAMP()
            """, (DB_MAC, addr, ieee))
            cur.close()
        except Exception as e:
            print(f"[DB] zigbee device upsert: {e}")

def _extract_value(cluster, raw):
    if cluster == "temperature":
        r = raw.get("raw")
        return None if (r is None or r == -32768) else r / 100.0
    if cluster == "humidity":     return raw.get("raw", 0) / 100.0
    if cluster == "illuminance":  return raw.get("raw")
    if cluster == "on_off":       return raw.get("v")
    if cluster == "occupancy":    return raw.get("occ")
    return None

def db_insert_zigbee_data(addr, cluster, raw):
    value = _extract_value(cluster, raw)
    with _db_lock:
        try:
            cur = _db_cursor()
            cur.execute("""
                INSERT INTO esp32_zigbee_data (mac, addr, cluster, ts, value, raw_json)
                VALUES (%s, %s, %s, UTC_TIMESTAMP(3), %s, %s)
            """, (DB_MAC, addr, cluster, value, json.dumps(raw)))
            cur.execute("""
                UPDATE esp32_zigbee_devices SET last_seen=UTC_TIMESTAMP()
                WHERE mac=%s AND addr=%s
            """, (DB_MAC, addr))
            cur.close()
        except Exception as e:
            print(f"[DB] zigbee data insert: {e}")

# ── MQTT ───────────────────────────────────────────────────────────────────
mq = mqtt.Client(client_id="serial-gateway")
if args.user:
    mq.username_pw_set(args.user, args.passwd)
mq.will_set(f"{BASE}/status", "offline", retain=True, qos=1)

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] verbunden → {BASE}")
        client.publish(f"{BASE}/status", "online", retain=True, qos=1)
        client.subscribe(f"{BASE}/cmd/+", qos=1)
        db_upsert_gateway("online")
        with _state_lock:
            _state["status"] = "online"
    else:
        print(f"[MQTT] Fehler rc={rc}")

def on_message(client, userdata, msg):
    """Kommandos vom Host → an C6 über Serial weiterleiten."""
    cmd = msg.topic.split("/")[-1]
    payload = msg.payload.decode()
    if cmd == "permit_join":
        uart_msg = json.dumps({"cmd": "permit_join", "sec": int(payload)}) + "\n"
        ser.write(uart_msg.encode())
        print(f"[→C6] {uart_msg.strip()}")
    elif cmd == "set_channel":
        ch = int(payload)
        if 11 <= ch <= 26:
            uart_msg = json.dumps({"cmd": "set_channel", "ch": ch}) + "\n"
            ser.write(uart_msg.encode())
            print(f"[→C6] {uart_msg.strip()}")
        else:
            print(f"[WARN] Ungültiger Zigbee-Kanal: {ch} (11-26)")
    elif cmd == "scan_chan":
        uart_msg = json.dumps({"cmd": "scan_chan"}) + "\n"
        ser.write(uart_msg.encode())
        print(f"[→C6] {uart_msg.strip()}")
    elif cmd == "switch2wifi":
        uart_msg = json.dumps({"cmd": "switch2wifi"}) + "\n"
        ser.write(uart_msg.encode())
        print(f"[→C6] {uart_msg.strip()}")
    elif cmd == "set_sleep":
        try:
            secs = max(10, int(payload))
            uart_msg = json.dumps({"cmd": "set_sleep", "s": secs}) + "\n"
            ser.write(uart_msg.encode())
            print(f"[→C6] {uart_msg.strip()}")
        except ValueError:
            print(f"[WARN] set_sleep: ungültiger Wert {payload}")

mq.on_connect = on_connect
mq.on_message = on_message

# ── Serial ─────────────────────────────────────────────────────────────────
class CoordinatorSerial:
    """Persistente Serial-Verbindung ohne ESP32-C6 Reset-Trigger."""
    def __init__(self, port, baudrate):
        # HUPCL deaktivieren bevor pyserial öffnet → WCH-Chip löst kein DTR-Reset aus
        import subprocess
        subprocess.run(["stty", "-F", port, "-hupcl"], check=False)
        self.ser = serial.Serial()
        self.ser.port     = port
        self.ser.baudrate = baudrate
        self.ser.timeout  = 1
        self.ser.dtr      = False
        self.ser.rts      = False
        self.ser.open()
        time.sleep(0.5)
        try:
            self.ser.dtr = False
        except Exception:
            pass
        print(f"[Serial] {port} @ {baudrate} offen (DTR/RTS=LOW, HUPCL=off)")

    def read(self, n):   return self.ser.read(n)
    def write(self, d):  return self.ser.write(d)
    def close(self):     self.ser.close()

    @property
    def in_waiting(self): return self.ser.in_waiting

# ── Dispatcher ─────────────────────────────────────────────────────────────
def dispatch(line: str):
    mq.publish(f"{BASE}/console", line, qos=0)

    try:
        msg = json.loads(line)
    except json.JSONDecodeError:
        return

    t = msg.get("t")
    if not t:
        return

    if t == "boot":
        mq.publish(f"{BASE}/status", "online", retain=True, qos=1)
        db_upsert_gateway("online")
        with _state_lock:
            _state["status"] = "online"
        print("[C6] boot")

    elif t == "vitals":
        p = msg.get("p")
        if p:
            mq.publish(f"{BASE}/mr60bha2", json.dumps(p))

    elif t == "zigbee":
        addr = "0x{:04x}".format(msg.get("addr", 0))
        sub  = msg.get("sub", "raw")
        p    = msg.get("p")
        if p is not None:
            topic = f"{BASE}/zigbee/{addr}/{sub}"
            mq.publish(topic, json.dumps(p))
            print(f"[MQTT] {topic} {p}")
            ieee = msg.get("ieee", "")
            db_upsert_zigbee_device(addr, ieee)
            db_insert_zigbee_data(addr, sub, p)
            with _state_lock:
                # Alten Eintrag mit gleicher IEEE entfernen (Adresse nach Rejoin geändert)
                if ieee:
                    old = [a for a, d in _state["devices"].items()
                           if d.get("ieee") == ieee and a != addr]
                    for a in old:
                        del _state["devices"][a]
                dev = _state["devices"].setdefault(addr, {
                    "ieee": ieee, "name": addr, "last_seen": "", "clusters": {}
                })
                dev["ieee"] = ieee
                # Temperatur-Cluster: 0x8000 ist Ungültig → raw:null
                if sub == "temperature" and isinstance(p, dict) and p.get("raw") == -32768:
                    p = dict(p); p["raw"] = None
                dev["clusters"][sub] = {"payload": p, "ts": _now()}
                dev["last_seen"] = _now()

    elif t == "heartbeat":
        mq.publish(f"{BASE}/heartbeat", line, retain=True, qos=1)
        db_upsert_gateway("online")
        with _state_lock:
            _state.update({
                "status":  "online",
                "uptime":  msg.get("uptime", 0),
                "channel": msg.get("ch", 0),
                "pan":     msg.get("pan", "–"),
            })
            for d in msg.get("dev", []):
                addr = d.get("addr")
                if addr:
                    dev = _state["devices"].setdefault(addr, {
                        "ieee": "", "name": addr, "last_seen": "", "clusters": {}
                    })
                    if "lqi"  in d: dev["lqi"]  = d["lqi"]
                    if "rssi" in d: dev["rssi"] = d["rssi"]
                    dev["last_seen"] = _now()
                    db_upsert_zigbee_device(addr)
        devs = msg.get("dev", [])
        lqi_str = " ".join(f"{d['addr']}={d['lqi']}" for d in devs) if devs else "–"
        print(f"[HB] uptime={msg.get('uptime')}s ch={msg.get('ch')} pan={msg.get('pan')} lqi=[{lqi_str}]")

    elif t == "permit_join":
        p = msg.get("p", {})
        if p:
            mq.publish(f"{BASE}/permit_join", json.dumps(p), qos=1)
            with _state_lock:
                _state["permit_join"] = p
            print(f"[MQTT] {BASE}/permit_join {p}")

    elif t == "scan":
        ch = msg.get("ch")
        if ch is not None:
            nets = msg.get("nets", [])
            db_insert_chan_scan(ch, msg.get("count", 0), json.dumps(nets))
            print(f"[SCAN] ch={ch} count={msg.get('count', 0)} nets={nets}")
        else:
            print(f"[SCAN] state={msg.get('state')}")

    elif t == "ota_req":
        _ota_handle_req(msg.get("off", 0), msg.get("sz", 64))

    elif t == "ota_status":
        st = msg.get("status", "?")
        with _state_lock:
            _state["ota"]["status"] = st
        print(f"[OTA] coordinator: {st} size={msg.get('size')} ver={msg.get('ver')}")

    elif t == "ota_srv_status":
        st = msg.get("status", "?")
        addr = msg.get("addr", "?")
        print(f"[OTA] client {addr}: {st}")
        if st == "done":
            with _state_lock:
                _state["ota"]["status"] = "done"
            _ota_close()


def _ota_start(bin_path: str):
    """Startet OTA: öffnet Binary, sendet ota_start an Coordinator."""
    global _ota_file
    import os as _os
    if not bin_path:
        bin_path = _os.path.join(
            _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))),
            "client", "build", "zigbee-client.bin"
        )
    if not _os.path.isfile(bin_path):
        print(f"[OTA] Datei nicht gefunden: {bin_path}")
        with _state_lock:
            _state["ota"]["status"] = "error"
        return
    size = _os.path.getsize(bin_path)
    # Versionsnummer aus Datei-Mtime ableiten (Unix-Timestamp, 32-bit)
    version = int(_os.path.getmtime(bin_path)) & 0xFFFFFFFF
    _ota_close()
    _ota_file = open(bin_path, "rb")
    with _state_lock:
        _state["ota"].update({
            "status":      "pending",
            "bin_path":    bin_path,
            "size":        size,
            "version":     version,
            "transferred": 0,
        })
    cmd = json.dumps({"cmd": "ota_start", "size": size, "ver": version})
    ser.write((cmd + "\n").encode())
    print(f"[OTA] gestartet: {bin_path} size={size} ver=0x{version:08x}")


def _ota_handle_req(offset: int, size: int):
    """Antwortet auf ota_req: liest Chunk aus Binärdatei und sendet hex-kodiert."""
    global _ota_file
    with _state_lock:
        ota = _state["ota"]
        active = ota["status"] in ("pending", "transferring")

    if not active or _ota_file is None:
        print(f"[OTA] ota_req off={offset} aber kein Transfer aktiv")
        return

    try:
        _ota_file.seek(offset)
        data = _ota_file.read(size)
        hex_data = data.hex()
        cmd = json.dumps({"cmd": "ota_data", "off": offset, "data": hex_data})
        ser.write((cmd + "\n").encode())
        with _state_lock:
            _state["ota"]["status"] = "transferring"
            _state["ota"]["transferred"] = offset + len(data)
    except Exception as e:
        print(f"[OTA] Fehler beim Lesen: {e}")
        _ota_close()
        with _state_lock:
            _state["ota"]["status"] = "error"


def _ota_close():
    global _ota_file
    if _ota_file:
        _ota_file.close()
        _ota_file = None

# ── Web-Dashboard ──────────────────────────────────────────────────────────
def _html():
    with _state_lock:
        s = json.loads(json.dumps(_state))   # deep copy

    online = s["status"] == "online"
    badge  = ("online" if online else "offline")

    pj     = s["permit_join"]
    pj_txt = f"🔓 offen ({pj.get('seconds',0)} s)" if pj.get("open") else "🔒 geschlossen"

    import os as _os
    default_bin = _os.path.join(
        _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))),
        "client", "build", "zigbee-client.bin"
    )

    dev_rows = ""
    for addr, d in s["devices"].items():
        clusters_html = ""
        for k, v in d["clusters"].items():
            if isinstance(v, dict) and "payload" in v:
                payload_str = json.dumps(v["payload"], indent=2, ensure_ascii=False)
                ts_str = v.get("ts", "")
            else:
                payload_str = json.dumps(v, indent=2, ensure_ascii=False)
                ts_str = ""
            clusters_html += (
                f'<div style="margin-bottom:.6em">'
                f'<b>{k}</b>'
                + (f' <span style="color:#888;font-size:.85em">{ts_str}</span>' if ts_str else "")
                + f'<pre style="margin:.2em 0 0 0;background:#0d0d1f;padding:.4em;'
                  f'white-space:pre-wrap;word-break:break-all;border-left:2px solid #4a6aaa">'
                  f'{payload_str}</pre></div>'
            )
        clusters_html = clusters_html or "<em>–</em>"
        lqi  = d.get("lqi",  "–")
        rssi = d.get("rssi", "–")
        lqi_html  = (f'<meter value="{lqi}" min="0" max="255" style="width:50px"></meter> {lqi}'
                     if isinstance(lqi, int) else "–")
        rssi_html = (f'{rssi} dBm' if isinstance(rssi, int) else "–")
        dev_rows += f"""
        <tr style="cursor:pointer" onclick="location.href='/device/{addr}'"
            title="Detail anzeigen">
          <td><a href="/device/{addr}" style="color:#7ecbff;text-decoration:none">{addr}</a></td>
          <td style="font-size:.85em;color:#aaa">{d.get("mfr","") or "–"}<br>{d.get("model","") or ""}</td>
          <td>{clusters_html}</td>
          <td>{lqi_html}</td>
          <td>{rssi_html}</td>
        </tr>"""

    return f"""<!DOCTYPE html>
<html lang="de"><head>
<meta charset="utf-8">
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
          padding:.3em .8em;cursor:pointer;border-radius:3px;margin-right:.4em}}
  button:hover{{background:#3a6aaa}}
  section{{background:#16213e;padding:1.2em;margin:1.2em 0;border-radius:6px}}
  footer{{margin-top:2em;color:#666;font-size:.85em}}
</style></head>
<body>
<h1>ESP32-C6 Zigbee Gateway</h1>
<p>Auto-Refresh 10 s &nbsp;|&nbsp; <a href="/api/state" style="color:#7ecbff">JSON</a></p>
<section>
  <h2>Coordinator <code>{BASE}</code>
    <span class="badge {badge}">{s['status']}</span>
  </h2>
  <table>
    <tr><th>Uptime</th><td>{s['uptime']} s</td></tr>
    <tr><th>Kanal</th><td>{s['channel']}</td></tr>
    <tr><th>PAN</th><td>{s['pan']}</td></tr>
    <tr><th>Permit Join</th><td>{pj_txt}</td></tr>
  </table>
  <h3>Permit Join</h3>
  <form method="POST" action="/api/permit_join">
    <button name="secs" value="180">🔓 180 s öffnen</button>
    <button name="secs" value="0">🔒 Schließen</button>
  </form>
  <h3>OTA Firmware-Update</h3>
  <table>
    <tr><th>Status</th><td>{s['ota']['status']}</td></tr>
    <tr><th>Binary</th><td>{s['ota']['bin_path'] or '–'}</td></tr>
    <tr><th>Fortschritt</th><td>{s['ota']['transferred']} / {s['ota']['size']} Byte</td></tr>
  </table>
  <form method="POST" action="/api/ota">
    <input name="path" value="{s['ota']['bin_path'] or default_bin}" size="60"
           style="background:#2a2a4a;color:#e0e0e0;border:1px solid #555;padding:.25em .4em">
    <button type="submit">⬆ OTA starten</button>
  </form>
  <h3>Zigbee-Geräte ({len(s['devices'])})</h3>
  <table>
    <tr><th>Adresse</th><th>Payload</th><th>LQI</th><th>RSSI</th></tr>
    {dev_rows or '<tr><td colspan="4"><em>keine Geräte</em></td></tr>'}
  </table>
</section>
<section>
  <h2>Raw State</h2>
  <pre style="background:#0d0d1f;padding:1em;overflow-x:auto;white-space:pre-wrap;word-break:break-all;font-size:.85em">{json.dumps(s, indent=2, ensure_ascii=False)}</pre>
</section>
<footer>
  Base: <code>{BASE}</code> &nbsp;|&nbsp; DB: <code>{DB_HOST}/{DB_NAME}</code>
</footer>
</body></html>"""


def _device_html(addr):
    """Detail-Seite für ein einzelnes Zigbee-Gerät."""
    with _state_lock:
        s = json.loads(json.dumps(_state))
    dev = s["devices"].get(addr)
    if dev is None:
        return None

    clusters_html = ""
    for k, v in dev["clusters"].items():
        if isinstance(v, dict) and "payload" in v:
            payload_str = json.dumps(v["payload"], indent=2, ensure_ascii=False)
            ts_str = v.get("ts", "")
        else:
            payload_str = json.dumps(v, indent=2, ensure_ascii=False)
            ts_str = ""
        clusters_html += f"""
        <section style="margin-bottom:1.2em">
          <h3 style="color:#7ecbff;margin-bottom:.4em">{k}
            {"<span style='color:#888;font-size:.8em;margin-left:.5em'>" + ts_str + "</span>" if ts_str else ""}
          </h3>
          <pre style="background:#0d0d1f;padding:1em;white-space:pre-wrap;
               word-break:break-all;border-left:3px solid #4a6aaa;margin:0;
               font-size:.9em">{payload_str}</pre>
        </section>"""

    lqi  = dev.get("lqi",  "–")
    rssi = dev.get("rssi", "–")
    lqi_bar = (f'<meter value="{lqi}" min="0" max="255" style="width:80px;vertical-align:middle"></meter> {lqi}'
               if isinstance(lqi, int) else "–")

    return f"""<!DOCTYPE html>
<html lang="de"><head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="15">
<title>Device {addr}</title>
<style>
  body{{background:#1a1a2e;color:#e0e0e0;font-family:monospace;margin:2em}}
  h1{{color:#00d4ff}} h2{{color:#a0c4ff;border-bottom:1px solid #333;padding-bottom:.3em}}
  h3{{color:#7ecbff;margin-top:1.2em}}
  table{{border-collapse:collapse;width:100%;margin:.5em 0}}
  th,td{{border:1px solid #444;padding:.4em .8em;text-align:left}}
  th{{background:#2a2a4a}} tr:nth-child(even){{background:#1e1e3a}}
  section{{background:#16213e;padding:1.2em;margin:1.2em 0;border-radius:6px}}
  a{{color:#7ecbff}}
  button{{background:#2a4a7a;color:#e0e0e0;border:1px solid #4a6a9a;
          padding:.3em .8em;cursor:pointer;border-radius:3px}}
  button:hover{{background:#3a6aaa}}
</style></head>
<body>
<p><a href="/">← Zurück zum Dashboard</a></p>
<h1>Device <code>{addr}</code></h1>
<section>
<table>
  <tr><th>Adresse</th><td>{addr}</td></tr>
  <tr><th>Name</th><td>{dev.get('name', addr)}</td></tr>
  <tr><th>IEEE</th><td>{dev.get('ieee', '–')}</td></tr>
  <tr><th>LQI</th><td>{lqi_bar}</td></tr>
  <tr><th>RSSI</th><td>{"" + str(rssi) + " dBm" if isinstance(rssi, int) else "–"}</td></tr>
  <tr><th>Zuletzt gesehen</th><td>{dev.get('last_seen', '–')}</td></tr>
</table>
</section>
<h2>Cluster-Payloads ({len(dev["clusters"])})</h2>
{clusters_html or "<em>keine Daten</em>"}
<section>
  <h2>Commands</h2>
  <form method="POST" action="/api/device/{addr}/cmd">
    <select name="cmd" style="background:#2a2a4a;color:#e0e0e0;border:1px solid #555;padding:.25em">
      <option value="switch2wifi">switch2wifi</option>
      <option value="leave">leave</option>
    </select>
    <button type="submit">Senden</button>
  </form>
  <h3 style="color:#7ecbff;margin-top:1em">Sleep-Intervall</h3>
  <form method="POST" action="/api/sleep">
    <input name="addr" type="hidden" value="{addr}">
    <input name="secs" type="number" value="600" min="10" max="86400"
           style="background:#2a2a4a;color:#e0e0e0;border:1px solid #555;
                  padding:.25em;width:80px"> Sekunden
    <button type="submit">Setzen</button>
  </form>
  <h3 style="color:#7ecbff;margin-top:1em">OTA Firmware-Update</h3>
  <form method="POST" action="/api/ota">
    <input name="path" value="" placeholder="leer = client/build/zigbee-client.bin" size="50"
           style="background:#2a2a4a;color:#e0e0e0;border:1px solid #555;padding:.25em .4em">
    <button type="submit">&#x2B06; OTA starten</button>
  </form>
</section>
<footer style="margin-top:2em;color:#666;font-size:.85em">
  Auto-Refresh 15 s &nbsp;|&nbsp; <a href="/api/state">JSON State</a>
</footer>
</body></html>"""


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass   # kein Access-Log im stdout

    def _send(self, code, ctype, body):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", len(data))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            self._send(200, "text/html; charset=utf-8", _html())
        elif path.startswith("/device/"):
            addr = path[len("/device/"):]
            page = _device_html(addr)
            if page:
                self._send(200, "text/html; charset=utf-8", page)
            else:
                self._send(404, "text/html; charset=utf-8",
                           f"<h2>Device {addr} nicht gefunden</h2><a href='/'>← zurück</a>")
        elif path == "/api/state":
            with _state_lock:
                self._send(200, "application/json", json.dumps(_state, indent=2))
        else:
            self._send(404, "text/plain", "Not Found")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        params = parse_qs(self.rfile.read(length).decode("utf-8", errors="replace"))
        path   = urlparse(self.path).path
        if path.startswith("/api/device/") and path.endswith("/cmd"):
            addr = path[len("/api/device/"):-len("/cmd")]
            cmd  = params.get("cmd", [""])[0]
            if cmd == "switch2wifi":
                uart_msg = json.dumps({"cmd": "switch2wifi"}) + "\n"
                ser.write(uart_msg.encode())
                print(f"[→C6] switch2wifi → {addr}")
            elif cmd == "leave":
                uart_msg = json.dumps({"cmd": "leave"}) + "\n"
                ser.write(uart_msg.encode())
            self.send_response(302)
            self.send_header("Location", f"/device/{addr}")
            self.end_headers()
            return

        if path == "/api/permit_join":
            secs = params.get("secs", ["0"])[0]
            mq.publish(f"{BASE}/cmd/permit_join", secs, qos=1)
            print(f"[Web] permit_join {secs}s")

        elif path == "/api/ota":
            bin_path = params.get("path", [""])[0].strip()
            _ota_start(bin_path)

        self.send_response(303)
        self.send_header("Location", "/")
        self.end_headers()


class _ReuseHTTPServer(HTTPServer):
    allow_reuse_address = True

def _run_web():
    srv = _ReuseHTTPServer(("0.0.0.0", WEB_PORT), _Handler)
    print(f"[Web] Dashboard → http://0.0.0.0:{WEB_PORT}/")
    srv.serve_forever()

# ── Start ───────────────────────────────────────────────────────────────────
db_init()

print(f"[Serial] öffne {args.port} @ {args.baud} …")
ser = CoordinatorSerial(args.port, args.baud)

print(f"[MQTT] verbinde {args.broker}:{args.mqport} …")
mq.connect(args.broker, args.mqport, keepalive=60)
mq.loop_start()

threading.Thread(target=_run_web, daemon=True).start()

# ── Hauptschleife ───────────────────────────────────────────────────────────
print("[Bridge] läuft – Ctrl+C zum Beenden")
buf = b""
try:
    while True:
        raw = ser.read(ser.in_waiting or 1)
        if raw:
            buf += raw
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if line:
                    decoded = line.decode("utf-8", errors="replace")
                    print(f"[C6] {decoded}")
                    dispatch(decoded)
except KeyboardInterrupt:
    print("\nBeendet.")
finally:
    mq.publish(f"{BASE}/status", "offline", retain=True, qos=1)
    db_upsert_gateway("offline")
    mq.loop_stop()
    ser.close()
