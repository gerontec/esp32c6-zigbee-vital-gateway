/*
 * ha_mqtt.c  –  UART-Serial-Implementierung (ersetzt MQTT)
 *
 * C6 GPIO16 (TX) → S3 GPIO44 (RX)
 * C6 GPIO17 (RX) ← S3 GPIO43 (TX)
 * GND ─────────── GND
 *
 * Protokoll: JSON-Lines (ein JSON-Objekt pro Zeile, \n-terminiert)
 *   C6 → S3: {"t":"boot"}
 *             {"t":"vitals","p":{...}}
 *             {"t":"zigbee","addr":<uint>,"sub":"<name>","p":{...}}
 *             {"t":"permit_join","p":{"open":true/false,"seconds":<n>}}
 *   S3 → C6: {"cmd":"permit_join","sec":<n>}
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "ha_mqtt.h"

#define TAG         "uart_serial"
#define UART_PORT   UART_NUM_0
#define UART_TX_PIN 16
#define UART_RX_PIN 17
#define UART_BAUD   115200
#define RX_BUF_SIZE 256

static mqtt_cmd_cb_t s_cmd_cb = NULL;

/* ── Hilfsfunktion: eine JSON-Zeile über UART senden ───────────────────── */
static void emit(const char *line) {
    uart_write_bytes(UART_PORT, line, strlen(line));
    uart_write_bytes(UART_PORT, "\n", 1);
}

/* ── RX-Task: Kommandos vom S3 empfangen ────────────────────────────────── */
static void rx_task(void *arg) {
    uint8_t *buf = malloc(RX_BUF_SIZE);
    configASSERT(buf);
    int pos = 0;

    while (1) {
        uint8_t c;
        int n = uart_read_bytes(UART_PORT, &c, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        if (c == '\n' || pos >= RX_BUF_SIZE - 1) {
            buf[pos] = '\0';
            /* Nur verarbeiten wenn Kommando und Callback registriert */
            if (pos > 2 && s_cmd_cb && strstr((char *)buf, "permit_join")) {
                /* Format: {"cmd":"permit_join","sec":180} */
                char *p = strstr((char *)buf, "\"sec\":");
                char secs_str[8] = "180";
                if (p) snprintf(secs_str, sizeof(secs_str), "%d", atoi(p + 6));
                s_cmd_cb("permit_join", secs_str, strlen(secs_str));
            }
            pos = 0;
        } else {
            buf[pos++] = c;
        }
    }
}

/* ── Öffentliche API ────────────────────────────────────────────────────── */

esp_err_t ha_mqtt_init(const char *broker_uri,
                       const char *username,
                       const char *password) {
    (void)broker_uri; (void)username; (void)password;

    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT,
                                        RX_BUF_SIZE * 2, 0, 0, NULL, 0));

    xTaskCreate(rx_task, "uart_rx", 2048, NULL, 4, NULL);

    emit("{\"t\":\"boot\"}");
    ESP_LOGI(TAG, "UART bereit: TX=GPIO%d RX=GPIO%d %d Baud",
             UART_TX_PIN, UART_RX_PIN, UART_BAUD);
    return ESP_OK;
}

void ha_mqtt_set_cmd_cb(mqtt_cmd_cb_t cb) {
    s_cmd_cb = cb;
}

void ha_mqtt_publish_vitals(const mr60_data_t *d) {
    char line[192];
    snprintf(line, sizeof(line),
        "{\"t\":\"vitals\",\"p\":{"
        "\"bpm\":%d,\"rpm\":%d,"
        "\"bpm_cat\":%u,\"rpm_cat\":%u,"
        "\"status\":%u,"
        "\"bpm_wave\":%.3f,\"rpm_wave\":%.3f}}",
        d->bpm, d->rpm,
        (unsigned)d->bpm_category, (unsigned)d->rpm_category,
        (unsigned)d->status,
        d->bpm_wave, d->rpm_wave);
    emit(line);
}

/* payload kommt als fertiges JSON-Objekt z.B. {"raw":2150} */
void ha_mqtt_publish_zigbee(uint16_t addr,
                             const char *subtopic,
                             const char *payload) {
    char line[160];
    snprintf(line, sizeof(line),
        "{\"t\":\"zigbee\",\"addr\":%u,\"sub\":\"%s\",\"p\":%s}",
        (unsigned)addr, subtopic, payload);
    emit(line);
}

void ha_mqtt_publish_permit_join(bool open, uint8_t seconds) {
    char line[80];
    snprintf(line, sizeof(line),
        "{\"t\":\"permit_join\",\"p\":{\"open\":%s,\"seconds\":%u}}",
        open ? "true" : "false", (unsigned)seconds);
    emit(line);
}

bool ha_mqtt_connected(void) {
    return true;  /* UART ist immer "verbunden" */
}

const char *ha_mqtt_base_topic(void) {
    return "c6/uart";
}

void ha_mqtt_logf(const char *tag, const char *fmt, ...) {
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGI(tag, "%s", msg);
}
