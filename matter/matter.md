# Matter over Thread – ESP32-C6 Vital Gateway

A second firmware variant for the same ESP32-C6 DevKit that publishes
MR60BHA2 vital-sign radar data via the **Matter over Thread** protocol
instead of MQTT/Zigbee.

```
MR60BHA2 ──UART──► ESP32-C6 ──Thread (802.15.4)──► Border Router ──IP──► Matter Controller
                                                                            (Apple Home / Google / Chip-tool)
```

---

## Why Matter over Thread?

| | Zigbee firmware | Matter/Thread firmware |
|---|---|---|
| Transport | WiFi + MQTT | Thread (802.15.4) mesh |
| Protocol | MQTT + HA auto-discovery | Matter (CHIP) Data Model |
| Commissioning | Fixed credentials in firmware | BLE GATT → QR code / NFC |
| Controller | Home Assistant only | Apple Home, Google Home, Chip-tool, … |
| OTA | manual flash | Matter OTA cluster |
| Sensor topic | `vital-gw-XXXX/mr60bha1` MQTT | Matter attribute reports |

Thread needs an **OpenThread Border Router** (e.g. Raspberry Pi + `ot-br-posix`)
to bridge Thread ↔ IP so the Matter controller can reach the device.

---

## Hardware (same as Zigbee variant)

| Component | Notes |
|---|---|
| ESP32-C6 DevKit | Integrated 802.15.4 radio (Thread + Zigbee share the same radio) |
| Seeed MR60BHA2 | 60 GHz mmWave radar, UART 115200 8N1, 3.3 V logic |
| Border Router | Raspberry Pi with `ot-br-posix` or a commercial Thread BR |

### Wiring (MR60BHA2 → ESP32-C6)

| MR60BHA2 pin | ESP32-C6 pin | Note |
|---|---|---|
| TX | GPIO 5 (RX1) | sensor → ESP |
| RX | GPIO 4 (TX1) | ESP → sensor |
| GND | GND | |
| 5V | 5V / VIN | sensor needs 5 V supply |

---

## MR60BHA2 Protocol

The MR60BHA2 uses a different binary frame format from the MR60BHA1:

```
[0x55][LEN_H][LEN_L][TYPE][CMD][DATA...][CRC]
```

| Field | Size | Description |
|---|---|---|
| SOF | 1 byte | Always `0x55` |
| LEN | 2 bytes | Big-endian count of TYPE+CMD+DATA bytes |
| TYPE | 1 byte | Function type (see below) |
| CMD | 1 byte | Command within type |
| DATA | 0–60 bytes | Payload |
| CRC | 1 byte | XOR of all bytes from TYPE through last DATA byte |

### Function types

| TYPE | Meaning | CMD | Payload |
|---|---|---|---|
| `0x05` | Work status | `0x01` | `0x00`=init `0x01`=calibrating `0x02`=measuring |
| `0x80` | Respiratory | `0x05` | uint8 breath rate (rpm) |
| `0x80` | Respiratory | `0x04` | uint8 category (0=none 1=normal 2=fast 3=slow) |
| `0x80` | Respiratory | `0x06` | float32 LE waveform |
| `0x81` | Heart rate | `0x05` | uint8 heart rate (bpm) |
| `0x81` | Heart rate | `0x04` | uint8 category (0=none 1=normal 2=fast 3=slow) |
| `0x81` | Heart rate | `0x06` | float32 LE waveform |

### Differences from MR60BHA1

| | MR60BHA1 | MR60BHA2 |
|---|---|---|
| SOF | `0x53 0x59` (2 bytes) | `0x55` (1 byte) |
| EOF | `0x54 0x43` (2 bytes) | none |
| LEN byte order | little-endian | big-endian |
| CRC algorithm | sum mod 256 | XOR |

The driver (`mr60bha2.c`) shares the same `mr60_data_t` struct and
`mr60_callback_t` callback type as the BHA1 driver, so upper-layer code
(endpoint, MQTT publish) is sensor-agnostic.

---

## Matter Data Model

Three endpoints are registered on the device:

| Endpoint | Type | Purpose |
|---|---|---|
| 0 | Root Node | Matter root (mandatory) |
| 1 | OccupancySensing | Radar presence → `Occupancy` attribute (1 = measuring) |
| 2 | Custom | Heart rate – cluster `0xFFF10001` |
| 3 | Custom | Breath rate – cluster `0xFFF10002` |

### Custom Vendor Clusters

Matter reserves cluster IDs `0xFC00–0xFFFE` for manufacturer-specific use.

#### Cluster 0xFFF10001 – HeartRate

| Attribute ID | Name | Type | Description |
|---|---|---|---|
| `0x0000` | value | uint16 | Heart rate in BPM |
| `0x0001` | category | uint8 | 0=none 1=normal 2=fast 3=slow |
| `0x0002` | radar_status | uint8 | 0=init 1=cal 2=measuring |

#### Cluster 0xFFF10002 – BreathRate

| Attribute ID | Name | Type | Description |
|---|---|---|---|
| `0x0000` | value | uint16 | Breath rate in breaths/min |
| `0x0001` | category | uint8 | 0=none 1=normal 2=fast 3=slow |
| `0x0002` | radar_status | uint8 | same as HeartRate cluster |

Attributes use `ATTRIBUTE_FLAG_EXTERNAL_STORAGE` so the Matter stack
reads current values on demand via `attribute::update()` rather than
caching stale data.

---

## Software Architecture

```
app_main()
│
├── nvs_flash_init()          NVS needed for Matter commissioning data
├── esp_netif_init()
├── esp_event_loop_create_default()
│
├── matter_endpoints_init()   Register endpoints + clusters, start Matter stack
│   ├── node::create()
│   ├── endpoint::occupancy_sensor::create()   ep 1
│   ├── endpoint::create()                     ep 2 (BPM)
│   ├── endpoint::create()                     ep 3 (RPM)
│   ├── vital_clusters_create()
│   │   ├── cluster::create(0xFFF10001)        HeartRate
│   │   └── cluster::create(0xFFF10002)        BreathRate
│   └── esp_matter::start()                   starts BLE commissioning + Thread
│
└── mr60bha2_init()           UART1, TX=GPIO4, RX=GPIO5, 115200 baud
    └── uart_task (FreeRTOS)  → frame parser → mr60_callback()
                                                │
                                                └── matter_update_vitals()
                                                    ├── attribute::update ep1 Occupancy
                                                    └── vital_clusters_update()
                                                        ├── attribute::update ep2 BPM/cat/status
                                                        └── attribute::update ep3 RPM/cat
```

### Source files

| File | Language | Description |
|---|---|---|
| `main/main.c` | C | `app_main`: init sequence, MR60 callback |
| `main/mr60bha2.c` | C | MR60BHA2 UART driver, frame parser, FreeRTOS task |
| `main/mr60bha2.h` | C | Shared with Zigbee firmware (symlink → `../../main/`) |
| `main/mr60bha1.c` | C | BHA1 driver (symlink → `../../main/`, kept for reference) |
| `main/matter_endpoint.cpp` | C++ | esp_matter endpoint + cluster registration, attribute updates |
| `main/matter_endpoint.h` | C | `extern "C"` API callable from `main.c` |
| `main/vital_cluster.cpp` | C++ | Create + update vendor-specific clusters 0xFFF10001/2 |
| `main/vital_cluster.h` | C | Cluster/attribute ID defines, `extern "C"` declarations |
| `main/CMakeLists.txt` | CMake | IDF component: C + C++ sources |
| `partitions.csv` | CSV | NVS + OTA partition table for 4 MB flash |
| `sdkconfig.defaults` | Kconfig | Build options (see below) |
| `../CMakeLists.txt` | CMake | Top-level project, EXTRA_COMPONENT_DIRS for esp-matter |

> `mr60bha2.c/h` are the canonical files in `main/`.
> `matter/main/mr60bha2.c/h` are symlinks so both firmware variants share one driver.

---

## Build Environment

| Tool | Version |
|---|---|
| ESP-IDF | v5.2.5 |
| esp-matter | v1.4 |
| connectedhomeip | bundled with esp-matter v1.4 |
| cmake | 3.30.2 (installed via `idf_tools.py`) |
| Target | ESP32-C6 |

### Build Script

```bash
bash ~/build_matter.sh
```

`build_matter.sh` does:
1. Sources `esp-idf/export.sh` and `esp-matter/export.sh`
2. Sets `TMPDIR=$HOME/tmp_build` (avoids filling 96 MB `/tmp` tmpfs)
3. Runs `idf.py set-target esp32c6 && idf.py build`
4. On success: `git add matter/ && git commit && git push`

### Manual Build

```bash
export IDF_PATH=~/esp-idf
export ESP_MATTER_PATH=~/esp-matter
export PATH=~/.espressif/tools/cmake/3.30.2/cmake-3.30.2-linux-x86_64/bin:$PATH
export TMPDIR=~/tmp_build

. $IDF_PATH/export.sh
. $ESP_MATTER_PATH/export.sh

cd esp32c6-repo/matter
idf.py set-target esp32c6
idf.py build
```

### Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## sdkconfig.defaults

Key options and why they are set:

| Option | Value | Reason |
|---|---|---|
| `CONFIG_OPENTHREAD_ENABLED` | `y` | Thread network stack |
| `CONFIG_OPENTHREAD_FTD` | `y` | Full Thread Device (router-capable) |
| `CONFIG_OPENTHREAD_RADIO_NATIVE` | `y` | Use ESP32-C6 built-in 802.15.4 radio |
| `CONFIG_BT_ENABLED` | `y` | BLE for Matter commissioning |
| `CONFIG_BT_NIMBLE_ENABLED` | `y` | NimBLE (lightweight BLE stack) |
| `CONFIG_ESP_MATTER_ENABLE_DATA_MODEL` | `y` | Matter data model |
| `CONFIG_ESP_MATTER_NVS_ENCRYPTION` | `n` | Simplified key storage (dev) |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | Use `partitions.csv` |
| `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` | `y` | 4 MB flash on ESP32-C6 DevKit |
| `CONFIG_CHIP_WIFI_NETWORK_ENDPOINT_ID` | `1` | Separate WiFi/Thread endpoint IDs (required by CHIP assert) |
| `CONFIG_ENABLE_WIFI_STATION` | `n` | Thread-only device, no WiFi |
| `CONFIG_ENABLE_WIFI_AP` | `n` | Thread-only device, no WiFi AP |
| `CONFIG_MBEDTLS_HKDF_C` | `y` | HKDF required by Matter crypto (CHIPCryptoPALmbedTLS) |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S` | `30` | Matter boot takes >5 s |

---

## Commissioning

1. Flash the firmware.
2. Open a Matter controller app (Apple Home, Google Home, chip-tool).
3. The device advertises via **BLE** (NimBLE) immediately after boot.
4. Scan the QR code (logged on serial) or enter the pairing code.
5. The controller provisions the Thread credentials over BLE.
6. After commissioning the device joins the Thread network and BLE stops.

### chip-tool example

```bash
# Commission (Thread dataset from your Border Router)
chip-tool pairing ble-thread 1 hex:<thread-dataset> 20202021 3840

# Read heart rate attribute
chip-tool any read-by-id 0xFFF10001 0x0000 1 2

# Read breath rate attribute
chip-tool any read-by-id 0xFFF10002 0x0000 1 3

# Subscribe to heart rate (report interval 0–30 s)
chip-tool any subscribe-by-id 0xFFF10001 0x0000 0 30 1 2
```

Default pairing code: `20202021`
Default discriminator: `3840`

---

## C / C++ Split and Linking

`main.c` is compiled as **C** (no `extern "C"` is implicit there).
`matter_endpoint.cpp` and `vital_cluster.cpp` are compiled as **C++** because
they call `esp_matter::` namespace APIs.

To allow `main.c` to call C++ functions:

- `matter_endpoint.h` wraps its declarations in `#ifdef __cplusplus / extern "C"`.
- The definitions in `matter_endpoint.cpp` are explicitly marked `extern "C"` so
  the C++ compiler emits unmangled symbol names.

```c
// matter_endpoint.h
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t matter_endpoints_init(void);
esp_err_t matter_update_vitals(...);
#ifdef __cplusplus
}
#endif
```

```cpp
// matter_endpoint.cpp
extern "C" esp_err_t matter_endpoints_init(void) { ... }
extern "C" esp_err_t matter_update_vitals(...)   { ... }
```

---

## Partition Table

```
# partitions.csv
nvs,         data, nvs,      0x9000,  0x6000   # runtime NVS
phy_init,    data, phy,      0xf000,  0x1000
nvs_keys,    data, nvs_keys, 0x10000, 0x1000
nvs_factory, data, nvs,      0x11000, 0x6000   # Matter commissioning data
otadata,     data, ota,      0x17000, 0x2000
ota_0,       app,  ota_0,    0x20000, 0x1C0000 # 1792 KB
ota_1,       app,  ota_1,    0x1E0000,0x1C0000 # 1792 KB
```

Total used: 3968 KB of 4096 KB available on the ESP32-C6 DevKit.

---

## Known Build Issues and Fixes

| Error | Root Cause | Fix |
|---|---|---|
| `static_assert(WIFI_ENDPOINT_ID != THREAD_ENDPOINT_ID)` | Both IDs default to 0 | `CONFIG_CHIP_WIFI_NETWORK_ENDPOINT_ID=1` + disable WiFi |
| `ESP_RETURN_ON_ERROR` undeclared | Missing `#include "esp_check.h"` | Added include to `mr60bha1.c` |
| `esp_err_t` undeclared in header | Missing `#include "esp_err.h"` | Added to `matter_endpoint.h` |
| `config_t` not member of `esp_matter::endpoint` | No generic `endpoint::config_t` in v1.4 | Use `endpoint::create(node, flags, NULL)` directly |
| `Id` not member of `esp_matter::cluster::occupancy_sensing` | Wrong namespace path | Use `chip::app::Clusters::OccupancySensing::Id` from ZAP headers |
| `CLUSTER_FLAG_SERVER` undeclared | Enum is in `esp_matter` namespace | `using namespace esp_matter` in .cpp files |
| `optional: No such file` in C compile | `.c` file includes C++ headers transitively | Renamed `matter_endpoint.c` → `.cpp`, `vital_cluster.c` → `.cpp` |
| `undefined reference to matter_update_vitals` | C++ name mangling | `extern "C"` on both declaration and definition |
| `undefined reference to mbedtls_hkdf` | HKDF disabled in mbedTLS | `CONFIG_MBEDTLS_HKDF_C=y` in `sdkconfig.defaults` |
| `/tmp` too small (96 MB tmpfs) | GCC temp files fill `/tmp` during Matter build | `export TMPDIR=$HOME/tmp_build` |
