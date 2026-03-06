#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "ha_mqtt.h"

#define TAG "ha_mqtt"

/* Basis-Topic: vital-gw/<MAC4> */
static char s_base[32];
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;

/* ── MQTT-Ereignishandler ───────────────────────────────────────────────── */
static void mqtt_event_handler(void *arg,
                                esp_event_base_t base,
                                int32_t event_id,
                                void *event_data) {
    esp_mqtt_event_handle_t ev = event_data;
    switch (ev->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_connected = true;
        /* Geräte-Verfügbarkeit */
        char avail[64];
        snprintf(avail, sizeof(avail), "%s/status", s_base);
        esp_mqtt_client_publish(s_client, avail, "online", 6, 1, true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        break;
    default:
        break;
    }
}

/* ── HA-Autodiscovery senden ────────────────────────────────────────────── */
static void publish_discovery(const char *component,  /* "sensor" */
                               const char *obj_id,
                               const char *name,
                               const char *state_topic,
                               const char *value_tpl,
                               const char *unit,
                               const char *icon,
                               const char *device_class) {
    char topic[128];
    snprintf(topic, sizeof(topic),
             "homeassistant/%s/%s/%s/config", component, s_base, obj_id);

    char payload[512];
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s\","
        "\"value_template\":\"%s\","
        "\"unit_of_measurement\":\"%s\","
        "\"icon\":\"%s\","
        "%s%s%s"
        "\"availability_topic\":\"%s/status\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"Vital Signs Gateway\","
            "\"model\":\"ESP32-C6 + MR60BHA1\","
            "\"manufacturer\":\"Gerontec\""
        "}"
        "}",
        name,
        s_base, obj_id,
        state_topic,
        value_tpl,
        unit,
        icon,
        device_class ? "\"device_class\":\"" : "",
        device_class ? device_class : "",
        device_class ? "\"," : "",
        s_base,
        s_base
    );

    esp_mqtt_client_publish(s_client, topic, payload,
                            strlen(payload), 1, true);
}

static bool s_discovery_sent = false;

static void send_vital_discovery(void) {
    if (s_discovery_sent) return;
    s_discovery_sent = true;

    char state_topic[64];
    snprintf(state_topic, sizeof(state_topic), "%s/mr60bha1", s_base);

    publish_discovery("sensor", "bpm", "Heart Rate",
        state_topic, "{{ value_json.bpm }}",
        "BPM", "mdi:heart-pulse", NULL);

    publish_discovery("sensor", "rpm", "Breathing Rate",
        state_topic, "{{ value_json.rpm }}",
        "/min", "mdi:lungs", NULL);

    publish_discovery("sensor", "bpm_cat", "Heart Rate Category",
        state_topic, "{{ value_json.bpm_category }}",
        "", "mdi:heart", NULL);

    publish_discovery("sensor", "rpm_cat", "Breathing Category",
        state_topic, "{{ value_json.rpm_category }}",
        "", "mdi:weather-windy", NULL);

    publish_discovery("sensor", "radar_status", "Radar Status",
        state_topic, "{{ value_json.status }}",
        "", "mdi:radar", NULL);

    ESP_LOGI(TAG, "HA discovery published for MR60BHA1");
}

/* ── Öffentliche API ────────────────────────────────────────────────────── */

esp_err_t ha_mqtt_init(const char *broker_uri,
                       const char *username,
                       const char *password) {
    /* Eindeutiges Basis-Topic aus MAC-Adresse */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_base, sizeof(s_base), "vital-gw-%02x%02x%02x%02x",
             mac[2], mac[3], mac[4], mac[5]);

    char lwt_topic[64];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", s_base);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri       = broker_uri,
        .credentials.username     = username,
        .credentials.authentication.password = password,
        .session.last_will.topic  = lwt_topic,
        .session.last_will.msg    = "offline",
        .session.last_will.qos    = 1,
        .session.last_will.retain = true,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client);
}

static const char *cat_str(uint8_t cat) {
    switch (cat) {
    case 1: return "normal";
    case 2: return "zu schnell";
    case 3: return "zu langsam";
    default: return "keine";
    }
}

static const char *status_str(uint8_t s) {
    switch (s) {
    case 0: return "initialisierung";
    case 1: return "kalibrierung";
    case 2: return "messung";
    default: return "unbekannt";
    }
}

void ha_mqtt_publish_vitals(const mr60_data_t *d) {
    if (!s_connected) return;
    send_vital_discovery();

    char topic[64];
    snprintf(topic, sizeof(topic), "%s/mr60bha1", s_base);

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{"
        "\"bpm\":%d,"
        "\"rpm\":%d,"
        "\"bpm_category\":\"%s\","
        "\"rpm_category\":\"%s\","
        "\"status\":\"%s\","
        "\"bpm_wave\":%.2f,"
        "\"rpm_wave\":%.2f"
        "}",
        d->bpm, d->rpm,
        cat_str(d->bpm_category),
        cat_str(d->rpm_category),
        status_str(d->status),
        d->bpm_wave, d->rpm_wave
    );

    esp_mqtt_client_publish(s_client, topic, payload,
                            strlen(payload), 0, false);
}

void ha_mqtt_publish_zigbee(uint16_t short_addr,
                             const char *cluster,
                             const char *payload) {
    if (!s_connected) return;
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/zigbee/0x%04x/%s",
             s_base, short_addr, cluster);
    esp_mqtt_client_publish(s_client, topic, payload,
                            strlen(payload), 0, false);
}

void ha_mqtt_publish_permit_join(bool open, uint8_t seconds) {
    if (!s_connected) return;
    char topic[64];
    char payload[32];
    snprintf(topic, sizeof(topic), "%s/permit_join", s_base);
    snprintf(payload, sizeof(payload),
             "{\"open\":%s,\"seconds\":%d}",
             open ? "true" : "false", seconds);
    esp_mqtt_client_publish(s_client, topic, payload,
                            strlen(payload), 0, false);
}

bool ha_mqtt_connected(void) {
    return s_connected;
}
