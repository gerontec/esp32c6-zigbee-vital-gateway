/*
 * ha_mqtt.c  –  UART0 (GPIO16 TX / GPIO17 RX) → CH340 → ttyACM0
 *
 * sdkconfig: CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
 *   UART0-Console ist deaktiviert, GPIO16/17 frei.
 *   gpio_reset_pin() stellt sicher, dass kein anderer Treiber die Pins hält.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "ha_mqtt.h"

#define UART_PORT   UART_NUM_0
#define UART_TX     GPIO_NUM_16
#define UART_RX     GPIO_NUM_17
#define UART_BAUD   115200
#define BUF_SIZE    1024

static mqtt_cmd_cb_t     s_cmd_cb   = NULL;
static SemaphoreHandle_t s_tx_mutex = NULL;

static int null_vprintf(const char *fmt, va_list args) {
    (void)fmt; (void)args;
    return 0;
}

static void emit(const char *line) {
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PORT, line, strlen(line));
    uart_write_bytes(UART_PORT, "\n", 1);
    xSemaphoreGive(s_tx_mutex);
}

static void rx_task(void *arg) {
    char *buf = malloc(BUF_SIZE);
    configASSERT(buf);
    int pos = 0;

    while (1) {
        uint8_t ch;
        int len = uart_read_bytes(UART_PORT, &ch, 1, pdMS_TO_TICKS(20));
        if (len <= 0) continue;

        if (ch == '\n' || ch == '\r') {
            if (pos > 0 && s_cmd_cb) {
                buf[pos] = '\0';
                if (strstr(buf, "permit_join")) {
                    char *p = strstr(buf, "\"sec\":");
                    char secs_str[8] = "180";
                    if (p) snprintf(secs_str, sizeof(secs_str), "%d", atoi(p + 6));
                    s_cmd_cb("permit_join", secs_str, strlen(secs_str));
                } else if (strstr(buf, "set_channel")) {
                    char *p = strstr(buf, "\"ch\":");
                    char ch_str[4] = "20";
                    if (p) snprintf(ch_str, sizeof(ch_str), "%d", atoi(p + 5));
                    s_cmd_cb("set_channel", ch_str, strlen(ch_str));
                } else if (strstr(buf, "scan_chan")) {
                    s_cmd_cb("scan_chan", "", 0);
                } else if (strstr(buf, "ota_start")) {
                    char *ps = strstr(buf, "\"size\":");
                    char *pv = strstr(buf, "\"ver\":");
                    char payload[32] = "";
                    if (ps) {
                        long sz  = atol(ps + 7);
                        long ver = pv ? atol(pv + 6) : 1;
                        snprintf(payload, sizeof(payload), "%ld,%ld", sz, ver);
                    }
                    s_cmd_cb("ota_start", payload, strlen(payload));
                } else if (strstr(buf, "ota_data")) {
                    char *pd = strstr(buf, "\"data\":\"");
                    if (pd) {
                        pd += 8;
                        char *end = strchr(pd, '"');
                        if (end) {
                            *end = '\0';
                            uint8_t byte_count = (uint8_t)((end - pd) / 2);
                            s_cmd_cb("ota_data", pd, byte_count);
                        }
                    }
                }
            }
            pos = 0;
        } else if (pos < BUF_SIZE - 1) {
            buf[pos++] = (char)ch;
        }
    }
}

esp_err_t ha_mqtt_init(const char *broker_uri,
                       const char *username,
                       const char *password) {
    (void)broker_uri; (void)username; (void)password;

    s_tx_mutex = xSemaphoreCreateMutex();
    configASSERT(s_tx_mutex);

    /* GPIO-Pins explizit zurücksetzen bevor UART-Treiber sie übernimmt */
    gpio_reset_pin(UART_TX);
    gpio_reset_pin(UART_RX);

    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX, UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT,
                                        BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));

    esp_log_set_vprintf(null_vprintf);

    xTaskCreate(rx_task, "uart_rx", 2048, NULL, 4, NULL);

    /* Kurz warten bis UART bereit */
    vTaskDelay(pdMS_TO_TICKS(100));
    emit("{\"t\":\"boot\"}");
    return ESP_OK;
}

void ha_mqtt_set_cmd_cb(mqtt_cmd_cb_t cb) { s_cmd_cb = cb; }

void ha_mqtt_publish_zigbee(uint16_t addr, const char *subtopic, const char *payload) {
    char line[160];
    snprintf(line, sizeof(line),
        "{\"t\":\"zigbee\",\"addr\":%u,\"sub\":\"%s\",\"p\":%s}",
        (unsigned)addr, subtopic, payload);
    emit(line);
}

void ha_mqtt_publish_heartbeat(uint32_t uptime_s, uint8_t channel, uint16_t pan_id) {
    char line[96];
    snprintf(line, sizeof(line),
        "{\"t\":\"heartbeat\",\"uptime\":%lu,\"ch\":%u,\"pan\":\"0x%04x\"}",
        (unsigned long)uptime_s, (unsigned)channel, (unsigned)pan_id);
    emit(line);
}

void ha_mqtt_publish_permit_join(bool open, uint8_t seconds) {
    char line[80];
    snprintf(line, sizeof(line),
        "{\"t\":\"permit_join\",\"p\":{\"open\":%s,\"seconds\":%u}}",
        open ? "true" : "false", (unsigned)seconds);
    emit(line);
}

bool        ha_mqtt_connected(void)          { return true; }
const char *ha_mqtt_base_topic(void)         { return "c6/uart"; }
void        ha_mqtt_logf(const char *t, const char *f, ...) { (void)t; (void)f; }
void        ha_mqtt_emit_raw(const char *line) { emit(line); }
