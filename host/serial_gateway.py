#!/usr/bin/env python3
"""
serial_gateway.py – liest JSON-Lines vom ESP32-C6 (COM-Port)
und published sie als MQTT an den Broker.

Verwendung:
    python3 serial_gateway.py
    python3 serial_gateway.py --port /dev/ttyACM1 --broker 192.168.178.218
"""
import argparse
import json
import sys
import time
import serial
import paho.mqtt.client as mqtt

# ── Konfiguration ─────────────────────────────────────────────────────────
DEFAULT_PORT   = "/dev/ttyACM0"
DEFAULT_BAUD   = 115200
DEFAULT_BROKER = "192.168.178.218"
DEFAULT_MQPORT = 1883
BASE_TOPIC     = None   # wird aus {"t":"boot"} oder MAC ermittelt

# ── Argumente ──────────────────────────────────────────────────────────────
ap = argparse.ArgumentParser()
ap.add_argument("--port",   default=DEFAULT_PORT)
ap.add_argument("--baud",   default=DEFAULT_BAUD,   type=int)
ap.add_argument("--broker", default=DEFAULT_BROKER)
ap.add_argument("--mqport", default=DEFAULT_MQPORT, type=int)
ap.add_argument("--user",   default=None)
ap.add_argument("--pass",   default=None, dest="passwd")
ap.add_argument("--base",   default="gw/c6serial",
                help="MQTT base topic (z.B. gw/aabbccdd)")
args = ap.parse_args()

BASE = args.base

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

print(f"[MQTT] verbinde {args.broker}:{args.mqport} …")
mq.connect(args.broker, args.mqport, keepalive=60)
mq.loop_start()

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

print(f"[Serial] öffne {args.port} @ {args.baud} …")
ser = CoordinatorSerial(args.port, args.baud)

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
        print("[C6] boot")

    elif t == "vitals":
        p = msg.get("p")
        if p:
            mq.publish(f"{BASE}/mr60bha2", json.dumps(p))
            print(f"[MQTT] {BASE}/mr60bha2 {p}")

    elif t == "zigbee":
        addr = msg.get("addr", 0)
        sub  = msg.get("sub", "raw")
        p    = msg.get("p")
        if p is not None:
            topic = f"{BASE}/zigbee/0x{addr:04x}/{sub}"
            mq.publish(topic, json.dumps(p))
            print(f"[MQTT] {topic} {p}")

    elif t == "heartbeat":
        mq.publish(f"{BASE}/heartbeat", line, retain=True, qos=1)
        print(f"[HB] uptime={msg.get('uptime')}s ch={msg.get('ch')} pan={msg.get('pan')}")

    elif t == "permit_join":
        p = msg.get("p")
        if p:
            mq.publish(f"{BASE}/permit_join", json.dumps(p), qos=1)
            print(f"[MQTT] {BASE}/permit_join {p}")

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
    mq.loop_stop()
    ser.close()
