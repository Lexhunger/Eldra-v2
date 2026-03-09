#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Initialize EXIO (for SD D3 enable) and mount SD card to /sdcard.
 *        Safe to call multiple times; subsequent calls return ESP_OK if already mounted.
 */
esp_err_t sd_driver_init(void);

/**
 * @brief Queue a directory listing (non-blocking for caller).
 * @param path Directory path, e.g. "/sdcard". NULL uses "/sdcard".
 */
esp_err_t sd_driver_list_async(const char *path);

/**
 * @brief Queue a file read and print to console.
 * @param path File path on SD.
 */
esp_err_t sd_driver_read_async(const char *path);

/**
 * @brief Queue file deletion.
 * @param path File path on SD.
 */
esp_err_t sd_driver_delete_async(const char *path);

/**
 * @brief Queue a log line to the daily rotating log.
 *        Logs are stored in /sdcard/LOGS/YYYYMMDD.log, only the latest 5 days kept.
 * @param line Text to append.
 */
esp_err_t sd_driver_log_async(const char *line);

/**
 * @brief Save Wi-Fi credentials to SD so they can be reused across boots.
 *        File is stored at /sdcard/WIFI/WIFI.CFG
 */
esp_err_t sd_driver_save_wifi_credentials(const char *ssid, const char *password);

/**
 * @brief Format the SD card (FAT). Re-mounts and recreates driver folders.
 *        WARNING: destructive — erases all files.
 */
esp_err_t sd_driver_format(void);

