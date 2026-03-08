-- ESP32-C6 Zigbee Vital Gateway – MariaDB Schema
-- Database: wagodb
-- Connection: -h 192.168.178.218 -u gh -p wagodb
--
-- Apply:
--   mysql -h 192.168.178.218 -u gh -p wagodb < schema.sql
--
-- All tables are created with IF NOT EXISTS so this file is
-- safe to run multiple times (idempotent).
-- ─────────────────────────────────────────────────────────────────────────────

-- ┌─────────────────────────────────────────────────────────────────────────┐
-- │ esp32_gateways                                                          │
-- │ One row per physical ESP32-C6 gateway device.                           │
-- │ Written by: MQTT topic vital-gw-XXXX/status  (online / offline / LWT)  │
-- │ Write pattern: UPSERT  (INSERT … ON DUPLICATE KEY UPDATE)               │
-- └─────────────────────────────────────────────────────────────────────────┘
CREATE TABLE IF NOT EXISTS esp32_gateways (
    mac        CHAR(8)      NOT NULL
                            COMMENT 'Last 4 bytes of WiFi MAC as hex, e.g. "a1b2c3d4". Primary key and prefix of all MQTT topics.',
    status     VARCHAR(16)  NOT NULL DEFAULT 'offline'
                            COMMENT '"online" or "offline". Set to "offline" by MQTT Last-Will-Testament on disconnect.',
    last_seen  DATETIME     NULL
                            COMMENT 'Timestamp of the last received status message (server time, second precision).',
    PRIMARY KEY (mac)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Registry of known ESP32-C6 gateway devices.';


-- ┌─────────────────────────────────────────────────────────────────────────┐
-- │ esp32_vitals                                                            │
-- │ Time-series of MR60BHA2 radar measurements.                            │
-- │ Written by: MQTT topic vital-gw-XXXX/mr60bha1                          │
-- │ Write pattern: INSERT only (never updated)                              │
-- └─────────────────────────────────────────────────────────────────────────┘
CREATE TABLE IF NOT EXISTS esp32_vitals (
    id           INT UNSIGNED AUTO_INCREMENT PRIMARY KEY
                 COMMENT 'Surrogate key, auto-increment.',
    mac          CHAR(8)         NOT NULL
                 COMMENT 'Gateway MAC – foreign key to esp32_gateways.mac.',
    ts           DATETIME(3)     NOT NULL
                 COMMENT 'Measurement timestamp with millisecond precision (NOW(3) on server).',
    bpm          SMALLINT UNSIGNED NULL
                 COMMENT 'Heart rate in beats per minute. NULL when sensor is initialising.',
    rpm          SMALLINT UNSIGNED NULL
                 COMMENT 'Breathing rate in breaths per minute. NULL when sensor is initialising.',
    bpm_cat      TINYINT UNSIGNED  NULL
                 COMMENT 'Heart-rate category: 0=none 1=normal 2=fast 3=slow.',
    rpm_cat      TINYINT UNSIGNED  NULL
                 COMMENT 'Breathing-rate category: 0=none 1=normal 2=fast 3=slow.',
    radar_status TINYINT UNSIGNED  NULL
                 COMMENT 'Sensor work-status: 0=init 1=calibrating 2=measuring.',
    INDEX idx_mac_ts (mac, ts)
        COMMENT 'Composite index for time-range queries per gateway.'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='MR60BHA2 radar vital-sign time series (heart rate + breathing rate).';


-- ┌─────────────────────────────────────────────────────────────────────────┐
-- │ esp32_zigbee_devices                                                    │
-- │ One row per Zigbee end-device that has ever joined the coordinator.     │
-- │ Written by: MQTT topic vital-gw-XXXX/zigbee/0xADDR/status (join event) │
-- │ Write pattern: UPSERT  (INSERT … ON DUPLICATE KEY UPDATE)               │
-- └─────────────────────────────────────────────────────────────────────────┘
CREATE TABLE IF NOT EXISTS esp32_zigbee_devices (
    mac        CHAR(8)      NOT NULL
               COMMENT 'Gateway that hosts this device.',
    addr       VARCHAR(8)   NOT NULL
               COMMENT 'Zigbee 16-bit short address, e.g. "0x1a2b". Assigned by the coordinator on join; may change after re-join.',
    ieee       VARCHAR(24)  NOT NULL DEFAULT ''
               COMMENT 'Zigbee 64-bit IEEE address, e.g. "0x00124b001234abcd". Globally unique per device.',
    name       VARCHAR(64)  NOT NULL DEFAULT ''
               COMMENT 'Human-friendly label set via the web UI (POST /api/device). Empty string if not yet named.',
    last_seen  DATETIME     NULL
               COMMENT 'Timestamp of the last sensor data frame from this device. Updated by db_insert_zigbee_data().',
    PRIMARY KEY (mac, addr)
        COMMENT 'Composite PK: same short address can appear on different gateways.'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Registry of Zigbee devices paired with any gateway.';


-- ┌─────────────────────────────────────────────────────────────────────────┐
-- │ esp32_zigbee_data                                                       │
-- │ Time-series of Zigbee sensor readings (all clusters, all devices).      │
-- │ Written by: MQTT topics vital-gw-XXXX/zigbee/0xADDR/<cluster>          │
-- │ Write pattern: INSERT only (never updated)                              │
-- └─────────────────────────────────────────────────────────────────────────┘
CREATE TABLE IF NOT EXISTS esp32_zigbee_data (
    id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY
               COMMENT 'Surrogate key, auto-increment.',
    mac        CHAR(8)      NOT NULL
               COMMENT 'Gateway MAC.',
    addr       VARCHAR(8)   NOT NULL
               COMMENT 'Zigbee short address of the reporting device.',
    cluster    VARCHAR(32)  NOT NULL
               COMMENT 'ZCL cluster name: temperature | humidity | illuminance | on_off | occupancy | raw.',
    ts         DATETIME(3)  NOT NULL
               COMMENT 'Measurement timestamp with millisecond precision (NOW(3) on server).',
    value      DOUBLE       NULL
               COMMENT 'Scaled numeric value: °C for temperature, % for humidity, lux for illuminance, 0/1 for on_off/occupancy. NULL for raw/unknown clusters.',
    raw_json   TEXT         NULL
               COMMENT 'Original JSON payload as received from MQTT, stored verbatim for traceability.',
    INDEX idx_mac_addr_cluster_ts (mac, addr, cluster, ts)
        COMMENT 'Composite index for time-range queries per device per cluster.'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Zigbee sensor time series (temperature, humidity, illuminance, on/off, occupancy).';
