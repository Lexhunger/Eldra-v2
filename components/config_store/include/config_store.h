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
    bool cloud_logs_console;
    char cloud_base_url[128];
    char cloud_token[64];
    int cloud_poll_interval_ms;
    int cloud_state_interval_ms;
    int cloud_log_interval_ms;
    int eyes_center_x_offset;
    int eyes_center_y_offset;
    int display_center_x_offset;
    int display_center_y_offset;
    int glyph_offset_x;
    int glyph_offset_y;
    int glyph_scale;
    int sleep_lid_depth;           // 0..6, sleepy lid base depth
    int angry_lid_depth;           // 0..6, angry lid base depth
    int sleep_start_hour;          // 0-23
    int sleep_end_hour;            // 0-23
    int sleep_window_inactivity_ms; // in-window inactivity before auto sleep
    int sleep_low_battery_pct;      // battery percentage threshold
    int sleep_global_inactivity_ms; // any-time inactivity before auto sleep
    int sleep_overfed_hold_ms;      // overfed+sleepy hold before auto sleep
    int mood_log_interval_minutes; // 0 = disable, otherwise minutes between emotion logs
    int affect_weight_happiness_pct; // 0..300 (100=default)
    int affect_weight_satiety_pct;   // 0..300 (100=default)
    int affect_weight_energy_pct;    // 0..300 (100=default)
    int affect_weight_social_pct;    // 0..300 (100=default)
    int affect_weight_fear_pct;      // 0..300 (100=default)
    int angry_dizzy_count_threshold; // trigger angry when dizzy count in window exceeds this
    int angry_dizzy_window_ms;       // rolling dizzy burst window in ms
    int angry_override_min_ms;       // timed angry hold minimum in ms
    int angry_override_max_ms;       // timed angry hold maximum in ms
    int mood_happiness;            // -1 means unused
    int mood_hunger;
    int mood_energy;
    int mood_social;
    int mood_fear;
    int mood_eldritch_charge;
    int mood_state;
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

/**
 * @brief Clear persisted presets (SD config + SD WiFi fallback + NVS mirrors)
 *        and rewrite defaults to CONFIG.CFG.
 */
esp_err_t config_store_factory_reset(void);

// NVS-backed mirror for display/eye offsets (fallback when SD config missing)
bool config_store_load_eye_offsets_nvs(int *disp_cx, int *disp_cy, int *eyes_cx, int *eyes_cy);
void config_store_save_eye_offsets_nvs(int disp_cx, int disp_cy, int eyes_cx, int eyes_cy);

// Mood mirror for power-loss persistence.
bool config_store_load_mood_nvs(int *hap, int *hun, int *eng, int *soc, int *fear, int *eld, int *state);
void config_store_save_mood_nvs(int hap, int hun, int eng, int soc, int fear, int eld, int state);

