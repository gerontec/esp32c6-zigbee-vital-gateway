-- ESP32-C6 Zigbee Gateway – MariaDB Schema
-- Datenbank: wagodb
-- Erstellt automatisch durch serial_gateway.py (CREATE TABLE IF NOT EXISTS)

CREATE TABLE IF NOT EXISTS `esp32_gateways` (
  `mac`       CHAR(8)      NOT NULL,
  `status`    VARCHAR(16)  NOT NULL DEFAULT 'offline',
  `last_seen` DATETIME     DEFAULT NULL,
  PRIMARY KEY (`mac`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Bekannte Zigbee-Geräte (upsert bei jedem Datenpaket)
CREATE TABLE IF NOT EXISTS `esp32_zigbee_devices` (
  `mac`       CHAR(8)      NOT NULL,
  `addr`      VARCHAR(8)   NOT NULL,
  `ieee`      VARCHAR(24)  NOT NULL DEFAULT '',
  `name`      VARCHAR(64)  NOT NULL DEFAULT '',
  `last_seen` DATETIME     DEFAULT NULL,
  PRIMARY KEY (`mac`, `addr`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Zeitreihe Zigbee-Sensorwerte
CREATE TABLE IF NOT EXISTS `esp32_zigbee_data` (
  `id`       INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  `mac`      CHAR(8)      NOT NULL,
  `addr`     VARCHAR(8)   NOT NULL,
  `cluster`  VARCHAR(32)  NOT NULL,
  `ts`       DATETIME(3)  NOT NULL,
  `value`    DOUBLE       DEFAULT NULL,
  `raw_json` TEXT         DEFAULT NULL,
  INDEX (`mac`, `addr`, `cluster`, `ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
