#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "ha_mqtt.h"

#define TAG "mqtt_bridge"

/* Basis-Topic: gw/<MAC4> */
static char s_base[24];
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static mqtt_cmd_cb_t s_cmd_cb = NULL;

/* ── Interne Hilfsfunktion: Topic publizieren ───────────────────────────── */
static void pub(const char *topic, const char *payload, int qos, int retain) {
    if (!s_connected) return;
    esp_mqtt_client_publish(s_client, topic, payload,
                            strlen(payload), qos, retain);
}

/* ── Kommando-Topic abonnieren ──────────────────────────────────────────── */
static void subscribe_cmds(void) {
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/cmd/+", s_base);
    esp_mqtt_client_subscribe(s_client, topic, 1);
    ESP_LOGI(TAG, "Subscribed: %s", topic);
}

/* ── MQTT-Ereignishandler ───────────────────────────────────────────────── */
static void mqtt_event_handler(void *arg,
                                esp_event_base_t base,
                                int32_t event_id,
                                void *event_data) {
    esp_mqtt_event_handle_t ev = event_data;

    switch (ev->event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected → %s", s_base);

        /* Verfügbarkeit melden */
        char avail[64];
        snprintf(avail, sizeof(avail), "%s/status", s_base);
        pub(avail, "online", 1, 1);

        /* Kommando-Topics abonnieren */
        subscribe_cmds();
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_DATA: {
        if (!s_cmd_cb) break;

        /* Topic: gw/<base>/cmd/<cmd_name> */
        char topic[128];
        int tlen = ev->topic_len < (int)(sizeof(topic) - 1)
                   ? ev->topic_len : (int)(sizeof(topic) - 1);
        memcpy(topic, ev->topic, tlen);
        topic[tlen] = '\0';

        /* Prüfen ob cmd-Topic */
        char cmd_prefix[64];
        snprintf(cmd_prefix, sizeof(cmd_prefix), "%s/cmd/", s_base);
        if (strncmp(topic, cmd_prefix, strlen(cmd_prefix)) == 0) {
            const char *cmd = topic + strlen(cmd_prefix);
            s_cmd_cb(cmd, ev->data, ev->data_len);
        }
        break;
    }

    default:
        break;
    }
}

/* ── Öffentliche API ────────────────────────────────────────────────────── */

esp_err_t ha_mqtt_init(const char *broker_uri,
                       const char *username,
                       const char *password) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_base, sizeof(s_base), "gw/%02x%02x%02x%02x",
             mac[2], mac[3], mac[4], mac[5]);

    char lwt_topic[64];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", s_base);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri                  = broker_uri,
        .credentials.username                = username,
        .credentials.authentication.password = password,
        .session.last_will.topic             = lwt_topic,
        .session.last_will.msg               = "offline",
        .session.last_will.qos               = 1,
        .session.last_will.retain            = true,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client);
}

void ha_mqtt_set_cmd_cb(mqtt_cmd_cb_t cb) {
    s_cmd_cb = cb;
}

/* Vitaldaten: rohe Integer, keine Strings für Kategorien */
void ha_mqtt_publish_vitals(const mr60_data_t *d) {
    char topic[64], payload[128];
    snprintf(topic, sizeof(topic), "%s/mr60bha1", s_base);
    snprintf(payload, sizeof(payload),
        "{\"bpm\":%d,\"rpm\":%d,"
        "\"bpm_cat\":%u,\"rpm_cat\":%u,"
        "\"status\":%u,"
        "\"bpm_wave\":%.3f,\"rpm_wave\":%.3f}",
        d->bpm, d->rpm,
        (unsigned)d->bpm_category, (unsigned)d->rpm_category,
        (unsigned)d->status,
        d->bpm_wave, d->rpm_wave);
    pub(topic, payload, 0, 0);
}

/* Zigbee-Daten: pass-through, payload kommt fertig von zb_gateway */
void ha_mqtt_publish_zigbee(uint16_t addr,
                             const char *subtopic,
                             const char *payload) {
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/zigbee/0x%04x/%s",
             s_base, addr, subtopic);
    pub(topic, payload, 0, 0);
}

/* Permit-Join-Status */
void ha_mqtt_publish_permit_join(bool open, uint8_t seconds) {
    char topic[64], payload[32];
    snprintf(topic,   sizeof(topic),   "%s/permit_join", s_base);
    snprintf(payload, sizeof(payload),
             "{\"open\":%s,\"seconds\":%u}",
             open ? "true" : "false", (unsigned)seconds);
    pub(topic, payload, 1, 0);
}

bool ha_mqtt_connected(void) {
    return s_connected;
}

const char *ha_mqtt_base_topic(void) {
    return s_base;
}

void ha_mqtt_logf(const char *tag, const char *fmt, ...) {
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* JSON-Sonderzeichen im msg escapen (nur " und \) */
    char escaped[256];
    int j = 0;
    for (int i = 0; msg[i] && j < (int)sizeof(escaped) - 2; i++) {
        if (msg[i] == '"' || msg[i] == '\\') escaped[j++] = '\\';
        escaped[j++] = msg[i];
    }
    escaped[j] = '\0';

    char topic[64], payload[320];
    snprintf(topic,   sizeof(topic),   "%s/debug", s_base);
    snprintf(payload, sizeof(payload), "{\"tag\":\"%s\",\"msg\":\"%s\"}", tag, escaped);
    pub(topic, payload, 0, 0);
    ESP_LOGI(tag, "%s", msg);
}
