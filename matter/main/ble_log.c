/* BLE UART Log Bridge – Nordic UART Service (NUS)
 *
 * Registriert NUS neben Matter's eigenem NimBLE-Stack.
 * ble_gatts_add_svcs() muss vor ble_gatts_start() (= vor esp_matter::start())
 * aufgerufen werden – Matter übernimmt die Services beim Starten von NimBLE.
 *
 * Verbindungs-Tracking erfolgt über ble_gap_event_listener_register(),
 * das beliebig viele parallele Listener erlaubt.
 */

#include "ble_log.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

#define TAG "ble_log"

/* ── NUS UUIDs (128-bit, little-endian) ──────────────────────────────────── */
/* Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E */
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);

/* TX Char 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (ESP→PC, NOTIFY) */
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);

/* RX Char 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (PC→ESP, WRITE) */
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);

/* ── State ───────────────────────────────────────────────────────────────── */
static uint16_t s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle = 0;
static bool     s_subscribed    = false;
static SemaphoreHandle_t s_mutex = NULL;

typedef int (*vprintf_like_t)(const char *, va_list);
static vprintf_like_t s_orig_vprintf = NULL;

/* ── GATT: RX (PC→ESP write) ─────────────────────────────────────────────── */
static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Empfangene Bytes als C-String ausgeben */
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        char buf[128];
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
        buf[len] = '\0';
        ESP_LOGI(TAG, "RX: %s", buf);
    }
    return 0;
}

/* ── GATT: TX (read/subscribe) ───────────────────────────────────────────── */
static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;  /* Notify-only; kein direktes Read nötig */
}

/* ── GATT Service Definition ─────────────────────────────────────────────── */
static const struct ble_gatt_svc_def s_nus_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   /* TX – ESP→PC */
                .uuid       = &NUS_TX_UUID.u,
                .access_cb  = nus_tx_access,
                .val_handle = &s_tx_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            {   /* RX – PC→ESP */
                .uuid      = &NUS_RX_UUID.u,
                .access_cb = nus_rx_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ── GAP Event Listener ──────────────────────────────────────────────────── */
static int nus_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_subscribed  = false;
            ESP_LOGI(TAG, "BLE connected  handle=%d", s_conn_handle);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == s_conn_handle) {
            ESP_LOGI(TAG, "BLE disconnected");
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_subscribed  = false;
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_subscribed = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "NUS TX notify %s",
                     s_subscribed ? "ENABLED" : "disabled");
        }
        break;

    default:
        break;
    }
    return 0;  /* Nicht konsumieren – Matter-Listener läuft parallel */
}

static struct ble_gap_event_listener s_nus_listener;

/* ── Log-Hook ────────────────────────────────────────────────────────────── */
void ble_log_write(const char *buf, size_t len)
{
    if (!s_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (s_tx_val_handle == 0) return;

    /* Sende in ≤20-Byte-Chunks (BLE Default MTU = 23, payload = 20) */
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 20) chunk = 20;

        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf + off, chunk);
        if (!om) break;
        /* ble_gatts_notify_custom gibt om frei */
        int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
        if (rc != 0) break;
        off += chunk;
    }
}

static int ble_vprintf_hook(const char *fmt, va_list args)
{
    int ret = 0;

    /* Original (UART) */
    if (s_orig_vprintf) {
        va_list copy;
        va_copy(copy, args);
        ret = s_orig_vprintf(fmt, copy);
        va_end(copy);
    }

    /* BLE – nur wenn jemand verbunden+subscribed */
    if (s_subscribed) {
        char buf[256];
        va_list copy2;
        va_copy(copy2, args);
        int n = vsnprintf(buf, sizeof(buf), fmt, copy2);
        va_end(copy2);
        if (n > 0) {
            ble_log_write(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
        }
    }

    return ret;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
esp_err_t ble_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    /* NUS-Dienst registrieren – VOR ble_gatts_start() (= vor Matter-Start) */
    int rc = ble_gatts_add_svcs(s_nus_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* GAP-Listener für Connect/Disconnect/Subscribe */
    rc = ble_gap_event_listener_register(&s_nus_listener, nus_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_event_listener_register failed: %d", rc);
        /* Nicht fatal – nur kein BLE-Tracking */
    }

    /* Log-Hook einhängen */
    s_orig_vprintf = esp_log_set_vprintf(ble_vprintf_hook);

    ESP_LOGI(TAG, "NUS BLE-Log-Service registriert");
    return ESP_OK;
}
