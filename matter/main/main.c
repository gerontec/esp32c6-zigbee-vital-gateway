#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

/* Wiederverwendet aus Zigbee-Variante (gleiche Hardware) */
#include "mr60bha1.h"
#include "matter_endpoint.h"

#define TAG "main"

/* MR60BHA1 UART-Pins – identisch zur Zigbee-Firmware */
#define MR60_UART    UART_NUM_1
#define MR60_TX_PIN  4
#define MR60_RX_PIN  5
#define MR60_BAUD    115200

static void mr60_callback(const mr60_data_t *d, void *ctx)
{
    ESP_LOGI(TAG, "bpm=%d rpm=%d cat=%d/%d status=%d",
             d->bpm, d->rpm, d->bpm_cat, d->rpm_cat, d->status);
    matter_update_vitals(d->bpm, d->rpm, d->bpm_cat, d->rpm_cat, d->status);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Matter over Thread – Vital Gateway");

    /* NVS (Matter benötigt NVS für Commissioning-Daten) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Matter-Endpoints registrieren und Stack starten */
    ESP_ERROR_CHECK(matter_endpoints_init());
    ESP_LOGI(TAG, "Matter Stack gestartet");

    /* MR60BHA1 Radar-Sensor (UART, gleiche Pins wie Zigbee-FW) */
    ESP_ERROR_CHECK(mr60_init(MR60_UART, MR60_TX_PIN, MR60_RX_PIN,
                               MR60_BAUD, mr60_callback, NULL));
    ESP_LOGI(TAG, "MR60BHA1 initialisiert");

    ESP_LOGI(TAG, "Bereit – warte auf Matter Commissioning (BLE)");
    /* Matter-Stack läuft in eigenen Tasks, main kann schlafen */
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
