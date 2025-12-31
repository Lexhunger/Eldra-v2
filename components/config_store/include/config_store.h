#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    bool auto_init_sd;
    bool auto_init_wifi;
    bool auto_init_rtc;
    bool auto_init_imu;
    bool auto_init_cloud;
    bool wifi_roam;
    char wifi_ssid[33];
    char wifi_pass[65];
    bool logs_to_sd;
    char cloud_base_url[128];
    char cloud_token[64];
    int eyes_center_x_offset;
    int eyes_center_y_offset;
} config_store_t;

/**
 * @brief Load config from /sdcard/config.json. If missing or malformed,
 *        write defaults and return them.
 */
esp_err_t config_store_load(config_store_t *out);

/**
 * @brief Save config to /sdcard/config.json.
 */
esp_err_t config_store_save(const config_store_t *cfg);

/**
 * @brief Get defaults used when no config file exists.
 */
void config_store_get_defaults(config_store_t *out);
