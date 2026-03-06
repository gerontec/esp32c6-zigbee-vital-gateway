#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zb_gateway.h"
#include "ha_mqtt.h"

#define TAG "zb_gw"
#define ZB_ENDPOINT 1

/* ── Gerätedatenbank (max 32 gepairte Geräte) ───────────────────────────── */
#define MAX_DEVICES 32
typedef struct {
    uint16_t short_addr;
    uint8_t  ieee[8];
    char     name[ZB_DEVICE_NAME_MAX];
    bool     used;
} zb_device_t;

static zb_device_t s_devices[MAX_DEVICES];
static SemaphoreHandle_t s_dev_mutex;

static zb_device_t *find_or_add(uint16_t addr, const uint8_t *ieee) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].used && s_devices[i].short_addr == addr)
            return &s_devices[i];
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].used) {
            s_devices[i].used = true;
            s_devices[i].short_addr = addr;
            if (ieee) memcpy(s_devices[i].ieee, ieee, 8);
            /* Standardname: Adresse */
            snprintf(s_devices[i].name, ZB_DEVICE_NAME_MAX,
                     "0x%04x", addr);
            return &s_devices[i];
        }
    }
    return NULL;
}

/* ── ZCL-Attribut-Callback (Gerätedaten empfangen) ─────────────────────── */
static esp_err_t zb_zcl_handler(const esp_zb_zcl_cmd_info_t *msg) {
    if (!msg) return ESP_ERR_INVALID_ARG;

    ESP_LOGD(TAG, "ZCL from 0x%04x cluster 0x%04x attr 0x%04x",
             msg->src_address.u.short_addr,
             msg->cluster, msg->field_sets->attr_id);

    /* On/Off-Cluster → MQTT publizieren */
    if (msg->cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        uint8_t val = *(uint8_t *)msg->field_sets->data;
        char payload[32];
        snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}",
                 val ? "ON" : "OFF");
        ha_mqtt_publish_zigbee(msg->src_address.u.short_addr,
                               "on_off", payload);
    }
    /* Temperatur-Cluster */
    else if (msg->cluster == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT) {
        int16_t raw = *(int16_t *)msg->field_sets->data;
        char payload[64];
        snprintf(payload, sizeof(payload),
                 "{\"temperature\":%.2f}", raw / 100.0f);
        ha_mqtt_publish_zigbee(msg->src_address.u.short_addr,
                               "temperature", payload);
    }
    /* Luftfeuchtigkeit */
    else if (msg->cluster == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT) {
        uint16_t raw = *(uint16_t *)msg->field_sets->data;
        char payload[64];
        snprintf(payload, sizeof(payload),
                 "{\"humidity\":%.2f}", raw / 100.0f);
        ha_mqtt_publish_zigbee(msg->src_address.u.short_addr,
                               "humidity", payload);
    }
    /* Beleuchtungsstärke */
    else if (msg->cluster == ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT) {
        uint16_t raw = *(uint16_t *)msg->field_sets->data;
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"lux\":%u}", raw);
        ha_mqtt_publish_zigbee(msg->src_address.u.short_addr,
                               "illuminance", payload);
    }
    /* Anwesenheit */
    else if (msg->cluster == ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING) {
        uint8_t occ = *(uint8_t *)msg->field_sets->data;
        char payload[32];
        snprintf(payload, sizeof(payload),
                 "{\"occupancy\":%s}", occ ? "true" : "false");
        ha_mqtt_publish_zigbee(msg->src_address.u.short_addr,
                               "occupancy", payload);
    }
    /* Alle anderen Cluster als Roh-JSON */
    else {
        char payload[64];
        snprintf(payload, sizeof(payload),
                 "{\"cluster\":\"0x%04x\",\"attr\":\"0x%04x\"}",
                 msg->cluster, msg->field_sets->attr_id);
        ha_mqtt_publish_zigbee(msg->src_address.u.short_addr,
                               "raw", payload);
    }
    return ESP_OK;
}

/* ── Netzwerk-Signal-Handler ────────────────────────────────────────────── */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *sg_p      = signal_struct->p_app_signal;
    esp_err_t err       = signal_struct->esp_err_status;
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
            esp_zb_ieee_addr_t pan_id;
            esp_zb_get_extended_pan_id(pan_id);
            ESP_LOGI(TAG,
                "Netzwerk gebildet – PAN 0x%04hx Kanal %d",
                esp_zb_get_pan_id(), esp_zb_get_current_channel());
            esp_zb_bdb_start_top_level_commissioning(
                ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Netzwerkbildung fehlgeschlagen, erneut versuchen");
            esp_zb_scheduler_alarm(
                (esp_zb_callback_t)esp_zb_bdb_start_top_level_commissioning,
                ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee bereit – Geräte können beitreten");
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *p =
            esp_zb_app_signal_get_params(sg_p);
        ESP_LOGI(TAG, "Neues Gerät: 0x%04hx", p->device_short_addr);
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        find_or_add(p->device_short_addr, p->ieee_addr);
        xSemaphoreGive(s_dev_mutex);
        /* MQTT-Meldung */
        char payload[64];
        snprintf(payload, sizeof(payload),
                 "{\"event\":\"joined\",\"addr\":\"0x%04x\"}",
                 p->device_short_addr);
        ha_mqtt_publish_zigbee(p->device_short_addr, "status", payload);
        break;
    }

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t *sec = esp_zb_app_signal_get_params(sg_p);
        bool open = (*sec > 0);
        ESP_LOGI(TAG, "Permit Join: %s (%d s)",
                 open ? "offen" : "geschlossen", *sec);
        ha_mqtt_publish_permit_join(open, *sec);
        break;
    }

    default:
        ESP_LOGD(TAG, "ZB Signal: %s (0x%x) %s",
                 esp_zb_zdo_signal_to_string(sig), sig,
                 esp_err_to_name(err));
        break;
    }
}

/* ── Zigbee-Haupttask ───────────────────────────────────────────────────── */
static void zb_task(void *arg) {
    esp_zb_cfg_t cfg = ESP_ZB_ZC_CONFIG();
    esp_zb_init(&cfg);

    esp_zb_on_off_switch_cfg_t sw_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
    esp_zb_ep_list_t *ep = esp_zb_on_off_switch_ep_create(ZB_ENDPOINT, &sw_cfg);
    esp_zb_device_register(ep);

    esp_zb_core_action_handler_register(zb_zcl_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_main_loop_iteration();
}

/* ── Öffentliche API ────────────────────────────────────────────────────── */
void zb_gateway_start(void) {
    memset(s_devices, 0, sizeof(s_devices));
    s_dev_mutex = xSemaphoreCreateMutex();

    esp_zb_platform_config_t pcfg = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&pcfg));
    xTaskCreate(zb_task, "zb_main", 8192, NULL, 5, NULL);
}

void zb_gateway_permit_join(uint8_t seconds) {
    esp_zb_bdb_open_network(seconds);
}

void zb_gateway_list_devices(void) {
    ESP_LOGI(TAG, "Gepairte Geräte:");
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].used) {
            ESP_LOGI(TAG, "  [%d] 0x%04x  \"%s\"",
                     i, s_devices[i].short_addr, s_devices[i].name);
        }
    }
    xSemaphoreGive(s_dev_mutex);
}

int zb_gateway_get_devices(zb_device_info_t *out, int max) {
    int count = 0;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES && count < max; i++) {
        if (s_devices[i].used) {
            out[count].short_addr = s_devices[i].short_addr;
            memcpy(out[count].ieee, s_devices[i].ieee, 8);
            strncpy(out[count].name, s_devices[i].name, ZB_DEVICE_NAME_MAX - 1);
            out[count].name[ZB_DEVICE_NAME_MAX - 1] = '\0';
            out[count].used = true;
            count++;
        }
    }
    xSemaphoreGive(s_dev_mutex);
    return count;
}

void zb_gateway_set_name(uint16_t short_addr, const char *name) {
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].used && s_devices[i].short_addr == short_addr) {
            strncpy(s_devices[i].name, name, ZB_DEVICE_NAME_MAX - 1);
            s_devices[i].name[ZB_DEVICE_NAME_MAX - 1] = '\0';
            break;
        }
    }
    xSemaphoreGive(s_dev_mutex);
}
