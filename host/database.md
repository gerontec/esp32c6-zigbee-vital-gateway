# MariaDB Persistence Layer

`gateway_service.py` persists all sensor data in a MariaDB database.
The schema consists of four tables: two **registries** (UPSERT pattern)
and two **time-series** (INSERT-only).

```
Connection: -h 192.168.178.218 -u gh -p wagodb
Apply DDL:  mysql -h 192.168.178.218 -u gh -p wagodb < schema.sql
```

---

## Table Overview

```
esp32_gateways          esp32_zigbee_devices
┌──────────────┐        ┌──────────────────────────┐
│ mac  PK      │◄──┐    │ mac+addr  PK             │◄──┐
│ status       │   │    │ ieee                     │   │
│ last_seen    │   │    │ name                     │   │
└──────────────┘   │    │ last_seen                │   │
                   │    └──────────────────────────┘   │
esp32_vitals        │    esp32_zigbee_data              │
┌──────────────┐   │    ┌──────────────────────────┐   │
│ id  PK AI    │   │    │ id  PK AI                │   │
│ mac          │───┘    │ mac                      │───┘
│ ts  (ms)     │        │ addr                     │
│ bpm          │        │ cluster                  │
│ rpm          │        │ ts  (ms)                 │
│ bpm_cat      │        │ value  (scaled)          │
│ rpm_cat      │        │ raw_json                 │
│ radar_status │        └──────────────────────────┘
└──────────────┘
```

---

## esp32_gateways

**Purpose:** One row per physical ESP32-C6 gateway. Written on every
`vital-gw-XXXX/status` MQTT message.

**Write pattern:** `INSERT … ON DUPLICATE KEY UPDATE` (UPSERT)

| Column | Type | Description |
|---|---|---|
| `mac` | `CHAR(8)` PK | Last 4 bytes of WiFi MAC in hex, e.g. `a1b2c3d4`. This is also the unique prefix of all MQTT topics published by this gateway. |
| `status` | `VARCHAR(16)` | `"online"` or `"offline"`. Set to `"offline"` automatically by the MQTT broker Last-Will-Testament when the TCP connection drops. |
| `last_seen` | `DATETIME` | Server timestamp of the last received status message (second precision). |

**Useful queries:**

```sql
-- All gateways and when they were last online
SELECT mac, status, last_seen FROM esp32_gateways ORDER BY last_seen DESC;

-- Gateways offline for more than 10 minutes
SELECT mac, last_seen
FROM esp32_gateways
WHERE status = 'offline' OR last_seen < NOW() - INTERVAL 10 MINUTE;
```

---

## esp32_vitals

**Purpose:** Time-series of MR60BHA2 radar measurements (heart rate + breathing rate).
Written on every `vital-gw-XXXX/mr60bha1` MQTT message.

**Write pattern:** `INSERT` only (rows are never modified)

| Column | Type | Description |
|---|---|---|
| `id` | `INT UNSIGNED` PK AI | Surrogate key. |
| `mac` | `CHAR(8)` | Gateway MAC → `esp32_gateways.mac`. |
| `ts` | `DATETIME(3)` | Server-side timestamp with **millisecond** precision (`NOW(3)`). |
| `bpm` | `SMALLINT UNSIGNED` | Heart rate in beats per minute. `NULL` while sensor is initialising. |
| `rpm` | `SMALLINT UNSIGNED` | Breathing rate in breaths per minute. `NULL` while sensor is initialising. |
| `bpm_cat` | `TINYINT UNSIGNED` | Heart-rate quality category: `0`=none `1`=normal `2`=fast `3`=slow. |
| `rpm_cat` | `TINYINT UNSIGNED` | Breathing-rate quality category: same codes as `bpm_cat`. |
| `radar_status` | `TINYINT UNSIGNED` | Sensor work state: `0`=init `1`=calibrating `2`=measuring. |

**Index:** `(mac, ts)` — for time-range queries per gateway.

**Useful queries:**

```sql
-- Last 10 vital readings for gateway a1b2c3d4
SELECT ts, bpm, rpm, bpm_cat, rpm_cat, radar_status
FROM esp32_vitals
WHERE mac = 'a1b2c3d4'
ORDER BY ts DESC LIMIT 10;

-- Average heart rate per minute (last hour)
SELECT
    DATE_FORMAT(ts, '%Y-%m-%d %H:%i:00') AS minute,
    ROUND(AVG(bpm), 1)  AS avg_bpm,
    ROUND(AVG(rpm), 1)  AS avg_rpm
FROM esp32_vitals
WHERE mac = 'a1b2c3d4'
  AND ts > NOW() - INTERVAL 1 HOUR
  AND radar_status = 2        -- only "measuring" frames
GROUP BY minute
ORDER BY minute;

-- Rows per day (data volume)
SELECT DATE(ts) AS day, COUNT(*) AS rows
FROM esp32_vitals
GROUP BY day ORDER BY day DESC;
```

---

## esp32_zigbee_devices

**Purpose:** Registry of every Zigbee end-device that has ever joined the
coordinator. One row per device. Written on `vital-gw-XXXX/zigbee/0xADDR/status`
join events and updated when new sensor data arrives.

**Write pattern:** `INSERT … ON DUPLICATE KEY UPDATE` (UPSERT)

| Column | Type | Description |
|---|---|---|
| `mac` | `CHAR(8)` | Gateway that hosts this device. Part of composite PK. |
| `addr` | `VARCHAR(8)` | Zigbee 16-bit short address, e.g. `0x1a2b`. Assigned by the coordinator on join. **May change** after a device re-joins. Part of composite PK. |
| `ieee` | `VARCHAR(24)` | Zigbee 64-bit IEEE (extended) address, e.g. `0x00124b001234abcd`. **Globally unique** per device and stable across re-joins. |
| `name` | `VARCHAR(64)` | Human-readable label. Set via the web UI (`POST /api/device`) or `devices.json`. Empty string until named. |
| `last_seen` | `DATETIME` | Updated every time a sensor-data frame is inserted into `esp32_zigbee_data`. |

**Primary key:** `(mac, addr)` — the same short address can appear on different
gateways. To uniquely identify a device across gateways use `ieee`.

**Useful queries:**

```sql
-- All paired devices with last activity
SELECT mac, addr, ieee, name, last_seen
FROM esp32_zigbee_devices
ORDER BY last_seen DESC;

-- Devices silent for more than 1 hour (possibly offline)
SELECT mac, addr, name, last_seen
FROM esp32_zigbee_devices
WHERE last_seen < NOW() - INTERVAL 1 HOUR;

-- Devices without a friendly name yet
SELECT mac, addr, ieee FROM esp32_zigbee_devices WHERE name = '';
```

---

## esp32_zigbee_data

**Purpose:** Time-series of all Zigbee sensor readings. One row per MQTT
message on any `vital-gw-XXXX/zigbee/0xADDR/<cluster>` topic.

**Write pattern:** `INSERT` only (rows are never modified)

| Column | Type | Description |
|---|---|---|
| `id` | `INT UNSIGNED` PK AI | Surrogate key. |
| `mac` | `CHAR(8)` | Gateway MAC. |
| `addr` | `VARCHAR(8)` | Zigbee short address of the reporting device. |
| `cluster` | `VARCHAR(32)` | ZCL cluster name: `temperature` · `humidity` · `illuminance` · `on_off` · `occupancy` · `raw` |
| `ts` | `DATETIME(3)` | Server-side timestamp with **millisecond** precision. |
| `value` | `DOUBLE` | Scaled numeric value — see table below. `NULL` for unknown (`raw`) clusters. |
| `raw_json` | `TEXT` | Original JSON payload from MQTT, stored verbatim for traceability and re-processing. |

### `value` scaling per cluster

| `cluster` | `value` formula | Example |
|---|---|---|
| `temperature` | `raw / 100.0` → °C | `2150` → `21.50` |
| `humidity` | `raw / 100.0` → % RH | `5500` → `55.00` |
| `illuminance` | raw lux (integer) | `320` → `320` |
| `on_off` | `1` = ON, `0` = OFF | |
| `occupancy` | `1` = occupied, `0` = clear | |
| `raw` | `NULL` (unknown cluster) | stored in `raw_json` only |

**Index:** `(mac, addr, cluster, ts)` — for time-range queries per device per cluster.

**Useful queries:**

```sql
-- Last temperature reading per device
SELECT z.name, d.addr, d.value, d.ts
FROM esp32_zigbee_data d
JOIN esp32_zigbee_devices z USING (mac, addr)
WHERE d.cluster = 'temperature'
  AND d.ts = (
      SELECT MAX(ts) FROM esp32_zigbee_data
      WHERE mac = d.mac AND addr = d.addr AND cluster = 'temperature'
  )
ORDER BY d.ts DESC;

-- Hourly average temperature for one device (last 24 h)
SELECT
    DATE_FORMAT(ts, '%Y-%m-%d %H:00') AS hour,
    ROUND(AVG(value), 2)              AS avg_temp_c,
    MIN(value)                        AS min_temp_c,
    MAX(value)                        AS max_temp_c
FROM esp32_zigbee_data
WHERE mac    = 'a1b2c3d4'
  AND addr   = '0x1a2b'
  AND cluster = 'temperature'
  AND ts > NOW() - INTERVAL 24 HOUR
GROUP BY hour
ORDER BY hour;

-- All ON/OFF events today
SELECT d.ts, z.name, d.addr, d.value AS state, d.raw_json
FROM esp32_zigbee_data d
JOIN esp32_zigbee_devices z USING (mac, addr)
WHERE d.cluster = 'on_off'
  AND DATE(d.ts) = CURDATE()
ORDER BY d.ts;

-- Data volume per cluster per day
SELECT DATE(ts) AS day, cluster, COUNT(*) AS rows
FROM esp32_zigbee_data
GROUP BY day, cluster
ORDER BY day DESC, rows DESC;
```

---

## Python Layer (`gateway_service.py`)

### Connection & Reconnect

```python
# Config (overridable via environment variables)
DB_HOST = os.getenv("DB_HOST", "192.168.178.218")
DB_USER = os.getenv("DB_USER", "gh")
DB_PASS = os.getenv("DB_PASS", "a12345")
DB_NAME = os.getenv("DB_NAME", "wagodb")
```

`_db_cursor()` calls `ping(reconnect=True)` before every statement so
transient network errors or idle-timeout disconnects are recovered automatically.

### Thread Safety

All DB calls run under a single `threading.Lock` (`_db_lock`) because
`gateway_service.py` uses multiple threads (MQTT callbacks, HTTP handlers,
watchdog). `pymysql` connections are not thread-safe by default.

### Functions

| Function | Table | Pattern | Trigger |
|---|---|---|---|
| `db_upsert_gateway(mac, status)` | `esp32_gateways` | UPSERT | `vital-gw-XXXX/status` MQTT |
| `db_insert_vitals(mac, data)` | `esp32_vitals` | INSERT | `vital-gw-XXXX/mr60bha1` MQTT |
| `db_upsert_zigbee_device(mac, addr, ieee, name)` | `esp32_zigbee_devices` | UPSERT | Zigbee join event |
| `db_insert_zigbee_data(mac, addr, cluster, raw, value)` | `esp32_zigbee_data` + update `last_seen` | INSERT | Any Zigbee sensor MQTT |

### Initialisation

`_db_init()` is called once at `main()` startup. It runs all four
`CREATE TABLE IF NOT EXISTS` statements so the schema is applied
automatically on first run — no manual DDL step required.

---

## Grafana / BI Integration

The tables are designed for direct use with Grafana's MySQL data source:

```sql
-- Grafana time-series panel: heart rate last 6 hours
SELECT
    ts          AS "time",
    bpm         AS "Heart Rate (BPM)",
    rpm         AS "Breath Rate (/min)"
FROM esp32_vitals
WHERE mac = 'a1b2c3d4'
  AND ts BETWEEN $__timeFrom() AND $__timeTo()
  AND radar_status = 2
ORDER BY ts;
```
