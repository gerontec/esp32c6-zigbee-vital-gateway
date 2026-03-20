/* BLE UART Log Bridge – Nordic UART Service (NUS)
 *
 * Problem: ble_gatts_add_svcs() muss NACH nimble_port_init() aber VOR
 * ble_gatts_start() aufgerufen werden. Beide laufen intern in
 * esp_matter::start() → kein direkter Hook möglich.
 *
 * Lösung: --wrap=nimble_port_init (siehe CMakeLists.txt).
 * __wrap_nimble_port_init ruft __real_nimble_port_init() dann sofort
 * ble_gatts_add_svcs(nus_svcs) auf – exakt das richtige Fenster.
 *
 * Log-Weiterleitung: esp_log_set_vprintf schreibt in einen Ring-Buffer.
 * Ein separater FreeRTOS-Task liest daraus und sendet via BLE Notify
 * (keine NimBLE-Calls im Log-Hook → kein Deadlock / Stack-Überlauf).
 *
 * Notebook verbindet sich mit:
 *   python3 ~/Desktop/ble_monitor.py
 */

#include "ble_log.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

#define TAG        "ble_log"
#define BUF_SIZE   4096   /* Ring-Buffer Größe */
#define CHUNK_SIZE   20   /* BLE MTU payload */

/* ── NUS UUIDs (128-bit, little-endian) ──────────────────────────────────── */
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);
static const ble_uuid128_t NUS_TX_UUID  = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);
static const ble_uuid128_t NUS_RX_UUID  = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);

/* ── State ───────────────────────────────────────────────────────────────── */
static uint16_t           s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static uint16_t           s_tx_val_handle = 0;
static bool               s_subscribed    = false;
static RingbufHandle_t    s_ringbuf       = NULL;

typedef int (*vprintf_like_t)(const char *, va_list);
static vprintf_like_t     s_orig_vprintf  = NULL;

/* ── GATT: RX (Notebook→ESP schreibt Befehle) ───────────────────────────── */
static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        char buf[128];
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
        buf[len] = '\0';
        ESP_LOGI(TAG, "BLE-RX: %s", buf);
    }
    return 0;
}

/* ── GATT: TX (notify-only, kein direktes Read) ─────────────────────────── */
static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{ return 0; }

/* ── GATT Service Definition ─────────────────────────────────────────────── */
static const struct ble_gatt_svc_def s_nus_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &NUS_TX_UUID.u, .access_cb = nus_tx_access,
              .val_handle = &s_tx_val_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &NUS_RX_UUID.u, .access_cb = nus_rx_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { 0 }
        },
    },
    { 0 }
};

/* ── GAP Listener (parallel zu Matter) ──────────────────────────────────── */
static struct ble_gap_event_listener s_listener;

static int nus_gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn_handle = ev->connect.conn_handle;
            s_subscribed  = false;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (ev->disconnect.conn.conn_handle == s_conn_handle) {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_subscribed  = false;
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_val_handle) {
            s_subscribed = (ev->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "NUS notify %s", s_subscribed ? "ON" : "off");
        }
        break;
    default: break;
    }
    return 0;
}

/* ── BLE-Sender-Task ─────────────────────────────────────────────────────── */
static void ble_sender_task(void *arg)
{
    for (;;) {
        size_t len;
        void *data = xRingbufferReceiveUpTo(s_ringbuf, &len,
                                             pdMS_TO_TICKS(100), CHUNK_SIZE);
        if (!data) continue;

        if (s_subscribed && s_conn_handle != BLE_HS_CONN_HANDLE_NONE
                         && s_tx_val_handle != 0) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
            if (om) {
                ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
            }
        }
        vRingbufferReturnItem(s_ringbuf, data);
    }
}

/* ── Log-Hook (NUR Ring-Buffer, kein NimBLE-Aufruf) ─────────────────────── */
static int ble_vprintf_hook(const char *fmt, va_list args)
{
    int ret = 0;
    if (s_orig_vprintf) {
        va_list c; va_copy(c, args); ret = s_orig_vprintf(fmt, c); va_end(c);
    }
    if (s_ringbuf && s_subscribed) {
        char buf[256];
        va_list c2; va_copy(c2, args);
        int n = vsnprintf(buf, sizeof(buf), fmt, c2);
        va_end(c2);
        if (n > 0) {
            size_t sz = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf)-1;
            xRingbufferSendFromISR(s_ringbuf, buf, sz, NULL);
        }
    }
    return ret;
}

/* ── nimble_port_init WRAP ───────────────────────────────────────────────── *
 * Wird vom Linker anstelle der echten nimble_port_init() aufgerufen.
 * Nach erfolgreichem NimBLE-Init registrieren wir sofort den NUS-Dienst —
 * das ist das einzige sichere Fenster vor ble_gatts_start().
 * ─────────────────────────────────────────────────────────────────────────── */
extern int __real_nimble_port_init(void);

int __wrap_nimble_port_init(void)
{
    int rc = __real_nimble_port_init();
    if (rc == 0) {
        int r = ble_gatts_add_svcs(s_nus_svcs);
        if (r != 0) {
            ESP_LOGW(TAG, "ble_gatts_add_svcs rc=%d", r);
        }
        ble_gap_event_listener_register(&s_listener, nus_gap_event, NULL);
        ESP_LOGI(TAG, "NUS GATT service registered");
    }
    return rc;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
esp_err_t ble_log_init(void)
{
    s_ringbuf = xRingbufferCreate(BUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_ringbuf) return ESP_ERR_NO_MEM;

    xTaskCreate(ble_sender_task, "ble_log_tx", 4096, NULL, 3, NULL);

    s_orig_vprintf = esp_log_set_vprintf(ble_vprintf_hook);
    ESP_LOGI(TAG, "BLE log bridge ready (NUS via --wrap nimble_port_init)");
    return ESP_OK;
}

void ble_log_write(const char *buf, size_t len)
{
    if (s_ringbuf && s_subscribed)
        xRingbufferSend(s_ringbuf, buf, len, 0);
}
