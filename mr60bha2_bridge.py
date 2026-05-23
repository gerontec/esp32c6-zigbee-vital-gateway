#!/usr/bin/env python3
"""
Seeed MR60BHA2 → heissa.de Bridge
Subscribes to all MR60BHA2 MQTT topics, assembles status JSON,
pushes to https://heissa.de/mr60bha2/api.php on every change.
"""

import json, time, requests
import paho.mqtt.client as mqtt

MQTT_BROKER = '192.168.178.218'
MQTT_PORT   = 1883
MQTT_CLIENT = 'mr60bha2_bridge'

API_URL = 'https://heissa.de/mr60bha2/api.php'
API_KEY = 'mr60_bridge_2026'

PREFIX = 'seeedstudio-mr60bha2-kit-0102dc'

TOPICS = [
    f'{PREFIX}/sensor/real-time_heart_rate/state',
    f'{PREFIX}/sensor/real-time_respiratory_rate/state',
    f'{PREFIX}/sensor/distance_to_detection_object/state',
    f'{PREFIX}/sensor/seeed_mr60bha2_illuminance/state',
    f'{PREFIX}/sensor/target_number/state',
    f'{PREFIX}/binary_sensor/person_information/state',
]

state = {}

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f'[MQTT] Connected to {MQTT_BROKER}')
        for t in TOPICS:
            client.subscribe(t)
        print(f'[MQTT] Subscribed to {len(TOPICS)} topics')
    else:
        print(f'[MQTT] Connection error rc={rc}')

def on_message(client, userdata, msg):
    val = msg.payload.decode().strip()
    topic = msg.topic

    if 'heart_rate' in topic:
        state['heart_rate'] = int(float(val))
    elif 'respiratory_rate' in topic:
        state['resp_rate'] = int(float(val))
    elif 'distance' in topic:
        state['distance'] = round(float(val), 2)
    elif 'illuminance' in topic:
        state['illuminance'] = round(float(val), 1)
    elif 'target_number' in topic:
        state['targets'] = int(float(val))
    elif 'person_information' in topic:
        state['person'] = val

    if len(state) >= 3:
        push_status()

def push_status():
    try:
        r = requests.post(API_URL, params={'action': 'push', 'key': API_KEY},
                          json=state, timeout=8)
        print(f'[API] pushed: HR={state.get("heart_rate")} RR={state.get("resp_rate")} dist={state.get("distance")} → {r.status_code}')
    except Exception as e:
        print(f'[API] error: {e}')

def main():
    c = mqtt.Client(client_id=MQTT_CLIENT)
    c.on_connect = on_connect
    c.on_message = on_message
    print(f'[Start] Connecting to {MQTT_BROKER}:{MQTT_PORT} …')
    c.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    c.loop_forever()

if __name__ == '__main__':
    main()
