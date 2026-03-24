#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_endpoint.h"
#include "zb_gateway.h"
#include "zb_ota_server.h"
#include "ha_mqtt.h"
#include "aps/esp_zigbee_aps.h"
#include "esp_ieee802154.h"

#define ZB_DEFAULT_CHANNEL  20
#define ZB_FALLBACK_CHANNEL 25
#define ZB_NVS_NAMESPACE    "zb_gw"
#define ZB_NVS_KEY_CHANNEL  "channel"

#define TAG "zb_gw"
#define ZB_ENDPOINT 1

/* ── Gerätedatenbank (nur Adress-Tracking, kein Name) ───────────────────── */
#define MAX_DEVICES 32
typedef struct {
    uint16_t short_addr;
    uint8_t  ieee[8];
    bool     used;
    uint8_t  lqi;   /* letzte LQI vom APS-Layer */
    int8_t   rssi;  /* letzte RSSI vom IEEE 802.15.4 Layer (dBm) */
} zb_device_t;

static zb_device_t  s_devices[MAX_DEVICES];
static SemaphoreHandle_t s_dev_mutex;
static volatile int8_t  s_last_rssi = 0;

static zb_device_t *find_or_add(uint16_t addr, const uint8_t *ieee) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].used && s_devices[i].short_addr == addr)
            return &s_devices[i];
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].used) {
            s_devices[i].used       = true;
            s_devices[i].short_addr = addr;
            if (ieee) memcpy(s_devices[i].ieee, ieee, 8);
            return &s_devices[i];
        }
    }
    ESP_LOGW(TAG, "Gerätedatenbank voll (max %d)", MAX_DEVICES);
    return NULL;
}

/* ── ZCL-Attribut-Callback ──────────────────────────────────────────────── */
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                    const void *message) {
    /* OTA Server: Bild-Query vom Client */
    if (callback_id == ESP_ZB_CORE_OTA_UPGRADE_SRV_QUERY_IMAGE_CB_ID) {
        const esp_zb_zcl_ota_upgrade_server_query_image_message_t *m = message;
        if (m && m->table_idx) *m->table_idx = 0;
        return ESP_OK;
    }

    /* OTA Server: Transfer-Status */
    if (callback_id == ESP_ZB_CORE_OTA_UPGRADE_SRV_STATUS_CB_ID) {
        const esp_zb_zcl_ota_upgrade_server_status_message_t *m = message;
        if (!m) return ESP_OK;
        const char *st =
            m->server_status == ESP_ZB_ZCL_OTA_UPGRADE_SERVER_STARTED ? "started" :
            m->server_status == ESP_ZB_ZCL_OTA_UPGRADE_SERVER_ABORTED ? "aborted" : "done";
        char line[80];
        snprintf(line, sizeof(line),
                 "{\"t\":\"ota_srv_status\",\"addr\":\"0x%04x\",\"status\":\"%s\"}",
                 m->zcl_addr.u.short_addr, st);
        ha_mqtt_emit_raw(line);
        ESP_LOGI(TAG, "OTA srv 0x%04x: %s", m->zcl_addr.u.short_addr, st);
        return ESP_OK;
    }

    if (callback_id != ESP_ZB_CORE_REPORT_ATTR_CB_ID &&
        callback_id != ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID)
        return ESP_OK;

    const esp_zb_zcl_report_attr_message_t *msg =
        (const esp_zb_zcl_report_attr_message_t *)message;
    if (!msg) return ESP_ERR_INVALID_ARG;

    uint16_t addr = msg->src_address.u.short_addr;

    ESP_LOGD(TAG, "ZCL 0x%04x cluster 0x%04x attr 0x%04x",
             addr, msg->cluster, msg->attribute.id);

    char payload[64];

    switch (msg->cluster) {
    case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF: {
        uint8_t v = *(uint8_t *)msg->attribute.data.value;
        snprintf(payload, sizeof(payload), "{\"v\":%u}", v);
        ha_mqtt_publish_zigbee(addr, "on_off", payload);
        break;
    }
    case ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT: {
        int16_t raw = *(int16_t *)msg->attribute.data.value;
        snprintf(payload, sizeof(payload), "{\"raw\":%d}", raw);
        ha_mqtt_publish_zigbee(addr, "temperature", payload);
        break;
    }
    case ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT: {
        uint16_t raw = *(uint16_t *)msg->attribute.data.value;
        snprintf(payload, sizeof(payload), "{\"raw\":%u}", raw);
        ha_mqtt_publish_zigbee(addr, "humidity", payload);
        break;
    }
    case ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT: {
        uint16_t raw = *(uint16_t *)msg->attribute.data.value;
        snprintf(payload, sizeof(payload), "{\"raw\":%u}", raw);
        ha_mqtt_publish_zigbee(addr, "illuminance", payload);
        break;
    }
    case ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING: {
        uint8_t occ = *(uint8_t *)msg->attribute.data.value;
        snprintf(payload, sizeof(payload), "{\"occ\":%u}", occ);
        ha_mqtt_publish_zigbee(addr, "occupancy", payload);
        break;
    }
    default: {
        snprintf(payload, sizeof(payload),
                 "{\"cluster\":\"0x%04x\",\"attr\":\"0x%04x\"}",
                 msg->cluster, msg->attribute.id);
        ha_mqtt_publish_zigbee(addr, "raw", payload);
        break;
    }
    }
    return ESP_OK;
}

/* ── Netzwerk-Signal-Handler ────────────────────────────────────────────── */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *sg_p = signal_struct->p_app_signal;
    esp_err_t err  = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = *sg_p;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        esp_zb_bdb_start_top_level_commissioning(
            ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Erster Start – Netzwerk wird gebildet");
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ESP_LOGI(TAG, "Neustart – Netzwerk wiederhergestellt");
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
        } else {
            ESP_LOGE(TAG, "Zigbee-Init fehlgeschlagen: %s",
                     esp_err_to_name(err));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Netzwerk gebildet – PAN 0x%04hx Kanal %d",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel());
            esp_zb_bdb_start_top_level_commissioning(
                ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Netzwerkbildung fehlgeschlagen – erneuter Versuch");
            esp_zb_scheduler_alarm(
                (esp_zb_callback_t)(void *)esp_zb_bdb_start_top_level_commissioning,
                ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee bereit – Permit Join geschlossen");
            esp_zb_bdb_open_network(0);   /* sofort schließen */
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *p =
            esp_zb_app_signal_get_params(sg_p);
        uint16_t a = p->device_short_addr;
        ESP_LOGI(TAG, "Neues Gerät: 0x%04hx", a);

        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        zb_device_t *dev = find_or_add(a, p->ieee_addr);
        xSemaphoreGive(s_dev_mutex);

        /* IEEE-Adresse als String */
        char ieee_str[24] = "?";
        if (dev) {
            snprintf(ieee_str, sizeof(ieee_str),
                     "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     dev->ieee[7], dev->ieee[6], dev->ieee[5], dev->ieee[4],
                     dev->ieee[3], dev->ieee[2], dev->ieee[1], dev->ieee[0]);
        }

        char payload[80];
        snprintf(payload, sizeof(payload),
                 "{\"event\":\"joined\",\"addr\":\"0x%04x\",\"ieee\":\"%s\"}",
                 a, ieee_str);
        ha_mqtt_publish_zigbee(a, "join", payload);
        break;
    }

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t *sec = esp_zb_app_signal_get_params(sg_p);
        bool open = (*sec > 0);
        ha_mqtt_logf(TAG, "PERMIT_JOIN_STATUS: %s (%d s)",
                     open ? "offen" : "geschlossen", *sec);
        ha_mqtt_publish_permit_join(open, *sec);
        break;
    }

    default:
        ESP_LOGD(TAG, "ZB-Signal: %s (0x%x) %s",
                 esp_zb_zdo_signal_to_string(sig), sig,
                 esp_err_to_name(err));
        break;
    }
}

/* ── Gespeicherten Kanal aus NVS lesen (Default: ZB_DEFAULT_CHANNEL) ─────── */
static uint8_t load_channel(void) {
    uint8_t ch = ZB_DEFAULT_CHANNEL;
    nvs_handle_t h;
    if (nvs_open(ZB_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, ZB_NVS_KEY_CHANNEL, &ch);
        nvs_close(h);
    }
    if (ch < 11 || ch > 26) ch = ZB_DEFAULT_CHANNEL;
    return ch;
}

static bool aps_data_ind_cb(esp_zb_apsde_data_ind_t ind);  /* fwd decl */

/* ── Zigbee-Haupttask ───────────────────────────────────────────────────── */
static void zb_task(void *arg) {
    uint8_t ch = load_channel();
    uint8_t fallback = (ch == ZB_FALLBACK_CHANNEL) ? ZB_DEFAULT_CHANNEL
                                                    : ZB_FALLBACK_CHANNEL;
    ESP_LOGI(TAG, "Zigbee primär Kanal %d, Fallback Kanal %d", ch, fallback);

    esp_zb_cfg_t cfg = {
        .esp_zb_role        = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg   = { .max_children = 10 },
    };
    esp_zb_init(&cfg);
    esp_zb_set_tx_power(20);   /* max TX-Power: 20 dBm */

    /* Endpoint manuell aufbauen: Basic + Identify + OTA-Server */
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = { .zcl_version = 3, .power_source = 0x01 };
    esp_zb_cluster_list_add_basic_cluster(cl,
        esp_zb_basic_cluster_create(&basic_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(cl,
        esp_zb_identify_cluster_create(&id_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_cluster_list_add_ota_cluster(cl,
        zb_ota_server_cluster_create(), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Temperature + On/Off als Client registrieren → empfängt Attribute-Reports */
    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = (int16_t)0x8000,
        .min_value = -5000,
        .max_value =  8500,
    };
    esp_zb_cluster_list_add_temperature_meas_cluster(cl,
        esp_zb_temperature_meas_cluster_create(&temp_cfg),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* Humidity Measurement (0x0405) CLIENT */
    esp_zb_humidity_meas_cluster_cfg_t hum_cfg = {
        .measured_value = 0, .min_value = 0, .max_value = 10000,
    };
    esp_zb_cluster_list_add_humidity_meas_cluster(cl,
        esp_zb_humidity_meas_cluster_create(&hum_cfg),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_on_off_cluster_cfg_t on_off_cfg = { .on_off = false };
    esp_zb_cluster_list_add_on_off_cluster(cl,
        esp_zb_on_off_cluster_create(&on_off_cfg),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* Illuminance (0x0400) */
    esp_zb_illuminance_meas_cluster_cfg_t illum_cfg = {
        .measured_value = 0, .min_value = 0, .max_value = 0xfffe,
    };
    esp_zb_cluster_list_add_illuminance_meas_cluster(cl,
        esp_zb_illuminance_meas_cluster_create(&illum_cfg), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* Occupancy Sensing (0x0406) – Bewegungsmelder */
    esp_zb_occupancy_sensing_cluster_cfg_t occ_cfg = {
        .occupancy = 0, .sensor_type = 0, .sensor_type_bitmap = 1,
    };
    esp_zb_cluster_list_add_occupancy_sensing_cluster(cl,
        esp_zb_occupancy_sensing_cluster_create(&occ_cfg), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* IAS Zone (0x0500) – PIR/Tür/Rauch-Sensoren */
    esp_zb_ias_zone_cluster_cfg_t ias_cfg = {
        .zone_state = 0, .zone_type = ESP_ZB_ZCL_IAS_ZONE_ZONETYPE_MOTION, .zone_status = 0,
    };
    esp_zb_cluster_list_add_ias_zone_cluster(cl,
        esp_zb_ias_zone_cluster_create(&ias_cfg), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* Power Configuration (0x0001) – Batterie */
    esp_zb_power_config_cluster_cfg_t pwr_cfg = { .main_voltage = 0 };
    esp_zb_cluster_list_add_power_config_cluster(cl,
        esp_zb_power_config_cluster_create(&pwr_cfg), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* Custom Cluster 0xFF01: switch2wifi-Befehl an Client (CLIENT-Role) */
    esp_zb_attribute_list_t *sw_attr = esp_zb_zcl_attr_list_create(0xFF01);
    esp_zb_cluster_list_add_custom_cluster(cl, sw_attr, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_ep_list_t *ep = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep, cl, ZB_ENDPOINT,
        ESP_ZB_AF_HA_PROFILE_ID, ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID);
    esp_zb_device_register(ep);

    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(1U << ch);
    esp_zb_set_secondary_network_channel_set(1U << fallback);

    esp_zb_aps_data_indication_handler_register(aps_data_ind_cb);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_main_loop_iteration();
}

/* ── Öffentliche API ────────────────────────────────────────────────────── */
void zb_gateway_start(void) {
    memset(s_devices, 0, sizeof(s_devices));
    s_dev_mutex = xSemaphoreCreateMutex();

    esp_zb_platform_config_t pcfg = {
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&pcfg));
    xTaskCreate(zb_task, "zb_main", 8192, NULL, 5, NULL);
}

void zb_gateway_permit_join(uint8_t seconds) {
    ha_mqtt_logf(TAG, "esp_zb_bdb_open_network(%u) called", seconds);
    esp_err_t err = esp_zb_bdb_open_network(seconds);
    ha_mqtt_logf(TAG, "esp_zb_bdb_open_network(%u) = 0x%x", seconds, err);
}

void zb_gateway_devices_json(char *buf, size_t len) {
    size_t pos = 0;
    pos += snprintf(buf + pos, len - pos, "[");
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool first = true;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].used) continue;
        const uint8_t *e = s_devices[i].ieee;
        pos += snprintf(buf + pos, len - pos,
            "%s{\"addr\":\"0x%04x\",\"ieee\":\"%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\"}",
            first ? "" : ",",
            s_devices[i].short_addr,
            e[7], e[6], e[5], e[4], e[3], e[2], e[1], e[0]);
        first = false;
    }
    xSemaphoreGive(s_dev_mutex);
    snprintf(buf + pos, len - pos, "]");
}

void zb_gateway_set_channel(uint8_t ch) {
    if (ch < 11 || ch > 26) {
        ESP_LOGE(TAG, "Ungültiger Kanal: %d (erlaubt: 11-26)", ch);
        return;
    }

    /* Kanal in eigenem NVS-Namespace speichern */
    nvs_handle_t h;
    if (nvs_open(ZB_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, ZB_NVS_KEY_CHANNEL, ch);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Kanal %d in NVS gespeichert", ch);
    } else {
        ESP_LOGE(TAG, "NVS-Schreibfehler");
        return;
    }

    /* Zigbee FAT-Storage löschen → nächster Boot bildet neues Netz */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "zb_storage");
    if (part) {
        esp_err_t err = esp_partition_erase_range(part, 0, part->size);
        ESP_LOGI(TAG, "zb_storage gelöscht: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "Partition zb_storage nicht gefunden");
    }

    ESP_LOGI(TAG, "Neustart für Kanalwechsel auf %d...", ch);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}


/* ── APS-Data-Indication: LQI je Device tracken ────────────────────────── */
static bool aps_data_ind_cb(esp_zb_apsde_data_ind_t ind) {
    int8_t rssi = esp_ieee802154_get_recent_rssi();
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    zb_device_t *d = find_or_add(ind.src_short_addr, NULL);
    if (d) {
        d->lqi  = (uint8_t)ind.lqi;
        d->rssi = rssi;
        s_last_rssi = rssi;
    }
    xSemaphoreGive(s_dev_mutex);
    return false;   /* nicht konsumiert */
}

/* LQI-JSON: [{"addr":"0x0684","lqi":220},...] */
void zb_gateway_lqi_json(char *buf, size_t len) {
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    char *p = buf;
    size_t r = len;
    int n = snprintf(p, r, "[");
    p += n; r -= (size_t)n;
    bool first = true;
    for (int i = 0; i < MAX_DEVICES && r > 4; i++) {
        if (!s_devices[i].used) continue;
        n = snprintf(p, r, "%s{\"addr\":\"0x%04x\",\"lqi\":%u,\"rssi\":%d}",
                     first ? "" : ",",
                     s_devices[i].short_addr,
                     (unsigned)s_devices[i].lqi,
                     (int)s_devices[i].rssi);
        p += n; r -= (size_t)n;
        first = false;
    }
    snprintf(p, r, "]");
    xSemaphoreGive(s_dev_mutex);
}

int8_t zb_gateway_get_last_rssi(void) { return s_last_rssi; }

void zb_gateway_list_devices(void) {
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "Gepairte Geräte:");
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].used) {
            ESP_LOGI(TAG, "  [%d] 0x%04x", i, s_devices[i].short_addr);
        }
    }
    xSemaphoreGive(s_dev_mutex);
}

/* ── Channel-Scan (Kanal 11-26, ~4s je Kanal) ──────────────────────────── */
/* forward decl: library exports zdo variant */
void esp_zb_zdo_active_scan_request(uint32_t channel_mask, uint8_t scan_duration, esp_zb_zdo_scan_complete_callback_t user_cb);

#define SCAN_CH_FIRST   11
#define SCAN_CH_LAST    26
#define SCAN_DURATION   7     /* 2^(7+1)-1 * 15.36 ms ≈ 3.9 s per channel */

static SemaphoreHandle_t s_scan_sem        = NULL;
static uint8_t           s_scan_cur_ch     = 0;
static uint8_t           s_scan_net_count  = 0;
static char              s_scan_net_buf[512];
static TaskHandle_t      s_scan_task_h     = NULL;

static void scan_cb(esp_zb_zdp_status_t status, uint8_t count,
                    esp_zb_network_descriptor_t *nwk) {
    /* Gefundene Netzwerke als JSON-Array bauen */
    char *p = s_scan_net_buf;
    int   r = (int)sizeof(s_scan_net_buf);
    int   n = snprintf(p, r, "[");
    p += n; r -= n;
    for (uint8_t i = 0; i < count && r > 4; i++) {
        n = snprintf(p, r, "%s{\"pan\":\"0x%04x\",\"open\":%s}",
                     i ? "," : "",
                     nwk[i].short_pan_id,
                     nwk[i].permit_joining ? "true" : "false");
        p += n; r -= n;
    }
    snprintf(p, r, "]");
    s_scan_net_count = count;
    xSemaphoreGive(s_scan_sem);
}

static void scan_task(void *arg) {
    char line[600];
    for (uint8_t ch = SCAN_CH_FIRST; ch <= SCAN_CH_LAST; ch++) {
        s_scan_cur_ch    = ch;
        s_scan_net_count = 0;
        strcpy(s_scan_net_buf, "[]");

        snprintf(line, sizeof(line),
                 "{\"t\":\"scan\",\"state\":\"scanning\",\"ch\":%d}", ch);
        ha_mqtt_emit_raw(line);

        esp_zb_zdo_active_scan_request(1U << ch, SCAN_DURATION, scan_cb);

        /* Auf Callback warten (Timeout 12 s) */
        xSemaphoreTake(s_scan_sem, pdMS_TO_TICKS(12000));

        snprintf(line, sizeof(line),
                 "{\"t\":\"scan\",\"ch\":%d,\"count\":%d,\"nets\":%s}",
                 ch, s_scan_net_count, s_scan_net_buf);
        ha_mqtt_emit_raw(line);

        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ha_mqtt_emit_raw("{\"t\":\"scan\",\"state\":\"done\"}");
    s_scan_task_h = NULL;
    vTaskDelete(NULL);
}

void zb_gateway_scan_channels(void) {
    if (s_scan_task_h) {
        ha_mqtt_emit_raw("{\"t\":\"scan\",\"state\":\"busy\"}");
        return;
    }
    if (!s_scan_sem) s_scan_sem = xSemaphoreCreateBinary();
    xTaskCreate(scan_task, "chan_scan", 4096, NULL, 2, &s_scan_task_h);
}


/* ── switch2wifi-Befehl an alle bekannten Devices senden ────────────────── */
#define SW2WIFI_CLUSTER  0xFF01
#define SW2WIFI_CMD_ID   0x01

static void send_switch2wifi_cb(uint8_t param) {
    (void)param;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    int sent = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].used) continue;
        esp_zb_zcl_custom_cluster_cmd_req_t req = {
            .zcl_basic_cmd = {
                .dst_addr_u  = { .addr_short = s_devices[i].short_addr },
                .dst_endpoint = 1,
                .src_endpoint = ZB_ENDPOINT,
            },
            .address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .profile_id    = ESP_ZB_AF_HA_PROFILE_ID,
            .cluster_id    = SW2WIFI_CLUSTER,
            .custom_cmd_id = SW2WIFI_CMD_ID,
            .direction     = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
            .data          = { .type = ESP_ZB_ZCL_ATTR_TYPE_NULL, .size = 0, .value = NULL },
        };
        esp_zb_zcl_custom_cluster_cmd_req(&req);
        char line[80];
        snprintf(line, sizeof(line),
                 "{\"t\":\"switch2wifi_cmd\",\"dst\":\"0x%04x\"}", s_devices[i].short_addr);
        ha_mqtt_emit_raw(line);
        sent++;
    }
    xSemaphoreGive(s_dev_mutex);
    if (sent == 0)
        ha_mqtt_emit_raw("{\"t\":\"switch2wifi_cmd\",\"error\":\"no_devices\"}");
}

void zb_gateway_send_switch2wifi_all(void) {
    esp_zb_scheduler_alarm(send_switch2wifi_cb, 0, 0);
}
