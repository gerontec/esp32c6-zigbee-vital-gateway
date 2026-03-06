#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "web_server.h"
#include "zb_gateway.h"
#include "ha_mqtt.h"
#include "mr60bha1.h"

#define TAG "web"

/* ── HTML-Template ──────────────────────────────────────────────────────── */
static const char HTML_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Vital-GW</title>"
    "<style>"
    "body{font-family:sans-serif;margin:0;background:#1a1a2e;color:#eee}"
    "h1{background:#16213e;padding:16px 24px;margin:0;font-size:1.2rem;"
    "   color:#e94560;letter-spacing:2px}"
    ".card{background:#16213e;border-radius:8px;padding:16px;margin:16px;"
    "      box-shadow:0 2px 8px #0005}"
    "h2{margin-top:0;font-size:1rem;color:#a8dadc}"
    "table{width:100%;border-collapse:collapse}"
    "th,td{text-align:left;padding:8px 10px;border-bottom:1px solid #0f3460}"
    "th{color:#a8dadc;font-weight:600}"
    "input[type=text]{background:#0f3460;border:1px solid #457b9d;color:#eee;"
    "                 padding:4px 8px;border-radius:4px;width:180px}"
    "button{background:#e94560;border:none;color:#fff;padding:5px 14px;"
    "       border-radius:4px;cursor:pointer}"
    "button:hover{background:#c73652}"
    ".pill{display:inline-block;padding:2px 8px;border-radius:12px;"
    "      font-size:.8rem}"
    ".ok{background:#2d6a4f;color:#95d5b2}"
    ".warn{background:#6d3a1e;color:#f4a261}"
    "</style></head><body>"
    "<h1>&#9760; VITAL SIGNS GATEWAY</h1>";

static const char HTML_TAIL[] =
    "<div class='card' style='font-size:.75rem;color:#555'>"
    "API: GET /api/devices &nbsp;|&nbsp; GET /api/vitals &nbsp;|&nbsp;"
    "POST /api/device (addr=0x1234&amp;name=Kueche)"
    "</div></body></html>";

/* ── Hilfsfunktionen ────────────────────────────────────────────────────── */

/* URL-Decode eines einzelnen Felds aus dem POST-Body */
static void url_decode(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (*src && i < max - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* Wert eines Query-/POST-Parameters extrahieren (format: key=value&...) */
static bool get_param(const char *body, const char *key,
                      char *out, size_t max) {
    size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if (p == body || *(p - 1) == '&') {
            if (p[klen] == '=') {
                p += klen + 1;
                size_t i = 0;
                while (*p && *p != '&' && i < max - 1) out[i++] = *p++;
                out[i] = '\0';
                return true;
            }
        }
        p++;
    }
    return false;
}

/* ── Handler: GET / ─────────────────────────────────────────────────────── */
static esp_err_t handler_root(httpd_req_t *req) {
    /* Vitaldaten */
    mr60_data_t vit;
    mr60_get(&vit);

    /* Geräteliste */
    static zb_device_info_t devs[32];
    int n = zb_gateway_get_devices(devs, 32);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, HTML_HEAD);

    /* ── Vitals-Karte ─────────────────────────────────────────────────── */
    char buf[512];
    const char *radar_class = (vit.status == 2) ? "ok" : "warn";
    const char *radar_txt   = (vit.status == 2) ? "messung" :
                              (vit.status == 1) ? "kalibrierung" : "init";
    snprintf(buf, sizeof(buf),
        "<div class='card'><h2>&#10084; Vitalwerte (MR60BHA1)</h2>"
        "<table><tr><th>Herzrate</th><td>%d BPM</td></tr>"
        "<tr><th>Atemrate</th><td>%d /min</td></tr>"
        "<tr><th>Status</th><td><span class='pill %s'>%s</span></td></tr>"
        "<tr><th>MQTT</th><td><span class='pill %s'>%s</span></td></tr>"
        "</table></div>",
        vit.bpm, vit.rpm,
        radar_class, radar_txt,
        ha_mqtt_connected() ? "ok" : "warn",
        ha_mqtt_connected() ? "verbunden" : "getrennt");
    httpd_resp_sendstr_chunk(req, buf);

    /* ── Geräte-Karte ─────────────────────────────────────────────────── */
    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>&#128268; Zigbee-Geräte</h2>"
        "<form method='POST' action='/api/device'>"
        "<table><tr><th>#</th><th>Adresse</th><th>IEEE</th>"
        "<th>Name</th><th></th></tr>");

    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf),
            "<tr>"
            "<td>%d</td>"
            "<td>0x%04x</td>"
            "<td style='font-size:.75rem'>%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x</td>"
            "<td>"
            "  <input type='hidden' name='_addr_%d' value='0x%04x'>"
            "  <input type='text' name='name_%d' value='%s' maxlength='31'>"
            "</td>"
            "<td><button type='submit' name='idx' value='%d'>&#10003;</button></td>"
            "</tr>",
            i + 1,
            devs[i].short_addr,
            devs[i].ieee[0], devs[i].ieee[1], devs[i].ieee[2], devs[i].ieee[3],
            devs[i].ieee[4], devs[i].ieee[5], devs[i].ieee[6], devs[i].ieee[7],
            i, devs[i].short_addr,
            i, devs[i].name,
            i);
        httpd_resp_sendstr_chunk(req, buf);
    }

    if (n == 0) {
        httpd_resp_sendstr_chunk(req,
            "<tr><td colspan='5' style='color:#555'>Keine Geräte verbunden</td></tr>");
    }

    httpd_resp_sendstr_chunk(req, "</table></form></div>");

    /* ── Permit-Join-Karte ────────────────────────────────────────────── */
    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>&#128279; Permit Join</h2>"
        "<form method='POST' action='/api/permit_join'>"
        "<button type='submit' name='dur' value='180'>180 s öffnen</button>"
        "&nbsp;"
        "<button type='submit' name='dur' value='0' style='background:#555'>"
        "Schließen</button>"
        "</form></div>");

    httpd_resp_sendstr_chunk(req, HTML_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);  /* EOF */
    return ESP_OK;
}

/* ── Handler: GET /api/devices ──────────────────────────────────────────── */
static esp_err_t handler_api_devices(httpd_req_t *req) {
    static zb_device_info_t devs[32];
    int n = zb_gateway_get_devices(devs, 32);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < n; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "%s{\"addr\":\"0x%04x\","
            "\"ieee\":\"%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\","
            "\"name\":\"%s\"}",
            i ? "," : "",
            devs[i].short_addr,
            devs[i].ieee[0], devs[i].ieee[1], devs[i].ieee[2], devs[i].ieee[3],
            devs[i].ieee[4], devs[i].ieee[5], devs[i].ieee[6], devs[i].ieee[7],
            devs[i].name);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── Handler: GET /api/vitals ───────────────────────────────────────────── */
static esp_err_t handler_api_vitals(httpd_req_t *req) {
    mr60_data_t d;
    mr60_get(&d);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"bpm\":%d,\"rpm\":%d,"
        "\"bpm_category\":%d,\"rpm_category\":%d,"
        "\"status\":%d,"
        "\"bpm_wave\":%.2f,\"rpm_wave\":%.2f,"
        "\"frames_ok\":%lu,\"frames_err\":%lu}",
        d.bpm, d.rpm,
        d.bpm_category, d.rpm_category,
        d.status,
        d.bpm_wave, d.rpm_wave,
        (unsigned long)d.frames_ok,
        (unsigned long)d.frames_err);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── Handler: POST /api/device (Name setzen) ────────────────────────────── */
static esp_err_t handler_api_device(httpd_req_t *req) {
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    /* idx des Eintrags */
    char idx_str[8];
    if (!get_param(body, "idx", idx_str, sizeof(idx_str))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing idx");
        return ESP_FAIL;
    }
    int idx = atoi(idx_str);

    /* Adresse und Name aus den indexierten Feldern */
    char addr_key[16], name_key[16];
    snprintf(addr_key, sizeof(addr_key), "_addr_%d", idx);
    snprintf(name_key, sizeof(name_key), "name_%d", idx);

    char addr_str[16], name_raw[64], name[ZB_DEVICE_NAME_MAX];
    if (!get_param(body, addr_key, addr_str, sizeof(addr_str)) ||
        !get_param(body, name_key, name_raw, sizeof(name_raw))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing fields");
        return ESP_FAIL;
    }

    url_decode(name, name_raw, sizeof(name));
    uint16_t short_addr = (uint16_t)strtol(addr_str, NULL, 16);
    zb_gateway_set_name(short_addr, name);

    ESP_LOGI(TAG, "Gerät 0x%04x umbenannt in \"%s\"", short_addr, name);

    /* Zurück zur Hauptseite */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/* ── Handler: POST /api/permit_join ────────────────────────────────────── */
static esp_err_t handler_permit_join(httpd_req_t *req) {
    char body[64] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len > 0) {
        body[len] = '\0';
        char dur_str[8];
        if (get_param(body, "dur", dur_str, sizeof(dur_str))) {
            uint8_t sec = (uint8_t)atoi(dur_str);
            zb_gateway_permit_join(sec);
            ESP_LOGI(TAG, "Permit Join: %d s", sec);
        }
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/* ── Server starten ─────────────────────────────────────────────────────── */
esp_err_t web_server_start(void) {
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port  = 80;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 8;

    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t routes[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = handler_root },
        { .uri = "/api/devices",     .method = HTTP_GET,  .handler = handler_api_devices },
        { .uri = "/api/vitals",      .method = HTTP_GET,  .handler = handler_api_vitals },
        { .uri = "/api/device",      .method = HTTP_POST, .handler = handler_api_device },
        { .uri = "/api/permit_join", .method = HTTP_POST, .handler = handler_permit_join },
    };
    for (int i = 0; i < (int)(sizeof(routes)/sizeof(routes[0])); i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "Webserver gestartet auf Port 80");
    return ESP_OK;
}
