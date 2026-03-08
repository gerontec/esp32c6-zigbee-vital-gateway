#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/uart.h"
#include "mr60bha2.h"

#define TAG "mr60bha2"

/*
 * MR60BHA2 frame format (host-receive direction):
 *   [0x55][LEN_H][LEN_L][TYPE][CMD][DATA…][CRC]
 *   LEN (big-endian) = sizeof(TYPE) + sizeof(CMD) + sizeof(DATA)
 *   CRC = XOR of bytes [TYPE … last DATA byte]
 *
 * Function types and commands (from Seeed protocol guide v1.0):
 *   TYPE 0x80  Respiratory monitoring
 *     CMD 0x05  Breath rate   data[0] = rpm (uint8)
 *     CMD 0x06  Breath wave   data[0..3] = float LE
 *     CMD 0x04  Breath cat    data[0] = MR60_CAT_*
 *   TYPE 0x81  Heart-rate monitoring
 *     CMD 0x05  Heart rate    data[0] = bpm (uint8)
 *     CMD 0x06  Heart wave    data[0..3] = float LE
 *     CMD 0x04  Heart cat     data[0] = MR60_CAT_*
 *   TYPE 0x05  Work status
 *     CMD 0x01  Status        data[0] = MR60_STATUS_*
 */
#define SOF          0x55
#define T_STATUS     0x05
#define T_BREATH     0x80
#define T_HEART      0x81
#define CMD_CATEGORY 0x04
#define CMD_RATE     0x05
#define CMD_WAVE     0x06

/* ── Parser state machine ────────────────────────────────────────────────── */
typedef enum {
    S_SOF, S_LENH, S_LENL, S_TYPE, S_CMD, S_DATA, S_CRC
} ps_t;

/* ── Module state ────────────────────────────────────────────────────────── */
static uart_port_t       s_port;
static mr60_callback_t   s_cb;
static TaskHandle_t      s_task  = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static mr60_data_t       s_data  = { .bpm = -1, .rpm = -1, .status = 0 };

static ps_t     s_state   = S_SOF;
static uint8_t  s_type, s_cmd;
static uint16_t s_len;          /* remaining payload bytes (TYPE+CMD+DATA) */
static uint16_t s_total_len;    /* original LEN field value */
static uint8_t  s_buf[64];
static uint16_t s_idx;
static uint8_t  s_crc_acc;

/* ── Frame dispatch ──────────────────────────────────────────────────────── */
static void dispatch(uint8_t t, uint8_t cmd, const uint8_t *data, uint16_t dlen)
{
    mr60_data_t upd;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    upd = s_data;
    xSemaphoreGive(s_mutex);

    bool changed = false;

    if (t == T_STATUS && cmd == 0x01 && dlen >= 1) {
        if (upd.status != data[0]) { upd.status = data[0]; changed = true; }
    } else if (t == T_BREATH) {
        if (cmd == CMD_RATE && dlen >= 1 && upd.rpm != (int)data[0]) {
            upd.rpm = data[0]; changed = true;
        } else if (cmd == CMD_CATEGORY && dlen >= 1 && upd.rpm_category != data[0]) {
            upd.rpm_category = data[0]; changed = true;
        } else if (cmd == CMD_WAVE && dlen >= 4) {
            float v; memcpy(&v, data, 4);
            if (v != upd.rpm_wave) { upd.rpm_wave = v; changed = true; }
        }
    } else if (t == T_HEART) {
        if (cmd == CMD_RATE && dlen >= 1 && upd.bpm != (int)data[0]) {
            upd.bpm = data[0]; changed = true;
        } else if (cmd == CMD_CATEGORY && dlen >= 1 && upd.bpm_category != data[0]) {
            upd.bpm_category = data[0]; changed = true;
        } else if (cmd == CMD_WAVE && dlen >= 4) {
            float v; memcpy(&v, data, 4);
            if (v != upd.bpm_wave) { upd.bpm_wave = v; changed = true; }
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (changed) s_data = upd;
    s_data.frames_ok++;
    xSemaphoreGive(s_mutex);
    if (changed && s_cb) s_cb(&upd);
}

/* ── Byte parser ─────────────────────────────────────────────────────────── */
static void parse_byte(uint8_t b)
{
    switch (s_state) {
    case S_SOF:
        if (b == SOF) s_state = S_LENH;
        break;
    case S_LENH:
        s_total_len = (uint16_t)b << 8;
        s_state = S_LENL;
        break;
    case S_LENL:
        s_total_len |= b;
        /* LEN includes TYPE (1) + CMD (1) + DATA (n); must be >= 2 */
        if (s_total_len < 2 || s_total_len > 62) {
            s_state = S_SOF;
            xSemaphoreTake(s_mutex, portMAX_DELAY); s_data.frames_err++; xSemaphoreGive(s_mutex);
        } else {
            s_len = s_total_len;
            s_idx = 0;
            s_crc_acc = 0;
            s_state = S_TYPE;
        }
        break;
    case S_TYPE:
        s_type = b;
        s_crc_acc ^= b;
        s_len--;
        s_state = S_CMD;
        break;
    case S_CMD:
        s_cmd = b;
        s_crc_acc ^= b;
        s_len--;
        s_state = (s_len > 0) ? S_DATA : S_CRC;
        break;
    case S_DATA:
        s_buf[s_idx++] = b;
        s_crc_acc ^= b;
        s_len--;
        if (s_len == 0) s_state = S_CRC;
        break;
    case S_CRC:
        s_state = S_SOF;
        if (b == s_crc_acc) {
            uint16_t dlen = (s_idx > 0) ? s_idx : 0;
            dispatch(s_type, s_cmd, s_buf, dlen);
        } else {
            xSemaphoreTake(s_mutex, portMAX_DELAY); s_data.frames_err++; xSemaphoreGive(s_mutex);
        }
        break;
    }
}

/* ── UART read task ──────────────────────────────────────────────────────── */
static void uart_task(void *arg)
{
    uint8_t buf[128];
    while (1) {
        int n = uart_read_bytes(s_port, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) parse_byte(buf[i]);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */
esp_err_t mr60bha2_init(uart_port_t port, int tx_pin, int rx_pin,
                         mr60_callback_t cb)
{
    s_port  = port;
    s_cb    = cb;
    s_mutex = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(port, 512, 0, 0, NULL, 0), TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(port, &cfg),                 TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(port, tx_pin, rx_pin,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),       TAG, "uart pins");

    xTaskCreate(uart_task, "mr60bha2", 2048, NULL, 5, &s_task);
    ESP_LOGI(TAG, "MR60BHA2 init ok (UART%d TX=%d RX=%d)", port, tx_pin, rx_pin);
    return ESP_OK;
}

void mr60bha2_deinit(void)
{
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    uart_driver_delete(s_port);
}

void mr60bha2_get(mr60_data_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_mutex);
}

bool mr60bha2_ready(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool r = (s_data.status == MR60_STATUS_MEAS &&
              s_data.bpm > 0 && s_data.rpm > 0);
    xSemaphoreGive(s_mutex);
    return r;
}
