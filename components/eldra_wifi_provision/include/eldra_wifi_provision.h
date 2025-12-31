#pragma once

#include "esp_err.h"
#include "esp_wifi.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ELDRA_WIFI_CONFIG_PATH "/sdcard/wifi.conf"

typedef struct {
    char ssid[33]; // 32-byte SSID + NUL
    char pass[65]; // 64-byte WPA2 passphrase + NUL
} eldra_wifi_credentials_t;

/**
 * @brief Ensure Wi-Fi creds file exists on SD card. When built with
 *        ELDRA_WIFI_PROV_WRITE, writes creds from compile-time defines if missing.
 */
esp_err_t eldra_wifi_provision_init(void);

/**
 * @brief Load Wi-Fi credentials from SD card.
 */
esp_err_t eldra_wifi_provision_load(eldra_wifi_credentials_t *out_creds);

/**
 * @brief Check if Wi-Fi credentials file exists.
 */
bool eldra_wifi_provision_file_exists(void);

/**
 * @brief Connect to Wi-Fi using credentials from SD card and log status.
 */
esp_err_t eldra_wifi_provision_connect(void);

/**
 * @brief Wait for the ongoing Wi-Fi connection attempt to finish.
 * @param timeout_ms Maximum time to wait.
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT on timeout, or other esp_err_t on failure.
 */
esp_err_t eldra_wifi_provision_wait(uint32_t timeout_ms);

/**
 * @brief Perform a blocking Wi-Fi scan and log visible SSIDs.
 */
void eldra_wifi_provision_scan_and_log(void);

/**
 * @brief Perform a blocking Wi-Fi scan and collect results.
 * @param records      Array of wifi_ap_record_t entries to fill.
 * @param max_records  Maximum entries that fit in records.
 * @param out_count    Number of entries written.
 */
esp_err_t eldra_wifi_provision_scan_collect(wifi_ap_record_t *records, size_t max_records, size_t *out_count);

#ifdef __cplusplus
}
#endif
