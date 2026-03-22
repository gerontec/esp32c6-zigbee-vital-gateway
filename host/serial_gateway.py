#!/usr/bin/env python3
"""
serial_gateway.py – liest JSON-Lines vom ESP32-C6 (COM-Port)
und published sie als MQTT an den Broker + schreibt in MariaDB.

Verwendung:
    python3 serial_gateway.py
    python3 serial_gateway.py --port /dev/ttyACM1 --broker 192.168.178.218
"""
import argparse
import json
import os
import sys
import threading
import time
import serial
import paho.mqtt.client as mqtt
import pymysql

# ── Konfiguration ─────────────────────────────────────────────────────────
DEFAULT_PORT   = "/dev/ttyACM0"
DEFAULT_BAUD   = 115200
DEFAULT_BROKER = "192.168.178.218"
DEFAULT_MQPORT = 1883

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

BASE    = args.base
DB_MAC  = BASE.split("/")[-1][:8]   # "coordinator" → "coordina", "b0a60487" bleibt

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
        print(f"[DB] verbunden {DB_HOST}/{DB_NAME} mac={DB_MAC}")
    except Exception as e:
        print(f"[DB] Fehler bei Init: {e}")
        _db_conn = None

def db_upsert_gateway(status):
    with _db_lock:
        try:
            cur = _db_cursor()
            cur.execute("""
                INSERT INTO esp32_gateways (mac, status, last_seen)
                VALUES (%s, %s, NOW())
                ON DUPLICATE KEY UPDATE status=VALUES(status), last_seen=NOW()
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
                VALUES (%s, %s, %s, NOW())
                ON DUPLICATE KEY UPDATE ieee=VALUES(ieee), last_seen=NOW()
            """, (DB_MAC, addr, ieee))
            cur.close()
        except Exception as e:
            print(f"[DB] zigbee device upsert: {e}")

def _extract_value(cluster, raw):
    if cluster == "temperature":  return raw.get("raw", 0) / 100.0
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
                VALUES (%s, %s, %s, NOW(3), %s, %s)
            """, (DB_MAC, addr, cluster, value, json.dumps(raw)))
            cur.execute("""
                UPDATE esp32_zigbee_devices SET last_seen=NOW()
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

mq.on_connect = on_connect
mq.on_message = on_message

# ── Serial ─────────────────────────────────────────────────────────────────
class CoordinatorSerial:
    """Persistente Serial-Verbindung ohne ESP32-C6 Reset-Trigger."""
    def __init__(self, port, baudrate):
        self.ser = serial.Serial()
        self.ser.port     = port
        self.ser.baudrate = baudrate
        self.ser.timeout  = 1
        self.ser.dtr      = False  # DTR LOW vor open → kein Reset
        self.ser.rts      = False  # RTS LOW vor open → kein Reset
        self.ser.open()
        time.sleep(0.5)
        self.ser.dtr = False       # nochmal nach open() sicherstellen
        print(f"[Serial] {port} @ {baudrate} offen (DTR/RTS=LOW)")

    def read(self, n):
        return self.ser.read(n)

    def write(self, data):
        return self.ser.write(data)

    @property
    def in_waiting(self):
        return self.ser.in_waiting

    def close(self):
        self.ser.close()

def dispatch(line: str):
    # Jede C6-Ausgabe (roh) für die Web-Konsole publishen
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
        print("[C6] boot")

    elif t == "vitals":
        p = msg.get("p")
        if p:
            mq.publish(f"{BASE}/mr60bha2", json.dumps(p))
            print(f"[MQTT] {BASE}/mr60bha2 {p}")

    elif t == "zigbee":
        addr = "0x{:04x}".format(msg.get("addr", 0))
        sub  = msg.get("sub", "raw")
        p    = msg.get("p")
        if p is not None:
            topic = f"{BASE}/zigbee/{addr}/{sub}"
            mq.publish(topic, json.dumps(p))
            print(f"[MQTT] {topic} {p}")
            db_upsert_zigbee_device(addr, msg.get("ieee", ""))
            db_insert_zigbee_data(addr, sub, p)

    elif t == "heartbeat":
        mq.publish(f"{BASE}/heartbeat", line, retain=True, qos=1)
        db_upsert_gateway("online")
        print(f"[HB] uptime={msg.get('uptime')}s ch={msg.get('ch')} pan={msg.get('pan')}")

    elif t == "permit_join":
        p = msg.get("p")
        if p:
            mq.publish(f"{BASE}/permit_join", json.dumps(p), qos=1)
            print(f"[MQTT] {BASE}/permit_join {p}")

# ── Start ───────────────────────────────────────────────────────────────────
db_init()

print(f"[MQTT] verbinde {args.broker}:{args.mqport} …")
mq.connect(args.broker, args.mqport, keepalive=60)
mq.loop_start()

print(f"[Serial] öffne {args.port} @ {args.baud} …")
ser = CoordinatorSerial(args.port, args.baud)

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
