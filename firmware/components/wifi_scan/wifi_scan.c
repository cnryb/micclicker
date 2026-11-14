#include "wifi_scan.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define MIC_WIFI_MAX_AP_RECORDS 32

static const char *TAG = "wifi_scan";

static esp_err_t wifi_scan_run(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // Existing pairing, calibration and user data must never be erased by a probe.
        ESP_LOGE(TAG, "NVS initialization failed without attempting erase: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (sta == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_ps: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Starting blocking 2.4 GHz scan; SSIDs and BSSIDs are not logged");
    err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t total = 0;
    err = esp_wifi_scan_get_ap_num(&total);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num: %s", esp_err_to_name(err));
        return err;
    }
    if (total == 0) {
        ESP_LOGE(TAG, "Scan completed but found no APs");
        esp_wifi_clear_ap_list();
        return ESP_ERR_NOT_FOUND;
    }

    wifi_ap_record_t records[MIC_WIFI_MAX_AP_RECORDS];
    memset(records, 0, sizeof(records));
    uint16_t stored = MIC_WIFI_MAX_AP_RECORDS;
    err = esp_wifi_scan_get_ap_records(&stored, records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        return err;
    }

    for (uint16_t i = 0; i < stored; ++i) {
        ESP_LOGI(TAG, "WIFI_AP index=%u channel=%u rssi=%d auth=%u",
                 (unsigned)i, (unsigned)records[i].primary, records[i].rssi,
                 (unsigned)records[i].authmode);
    }
    ESP_LOGI(TAG, "WIFI_SCAN: PASS total=%u stored=%u", (unsigned)total, (unsigned)stored);
    return ESP_OK;
}

esp_err_t micclicker_wifi_scan_run(void)
{
    esp_err_t err = wifi_scan_run();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WIFI_SCAN: FAIL error=%s", esp_err_to_name(err));
    }
    return err;
}
