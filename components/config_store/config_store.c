#include "config_store.h"
#include "eye_offsets_nvs.h"
#include "mood_nvs.h"
#include "eldra_logging.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_check.h"
#include "sys/stat.h"
#include "sys/unistd.h"
#include <errno.h>

#define CONFIG_DIR  "/sdcard/CONFIG"
#define CONFIG_PATH "/sdcard/CONFIG/CONFIG.CFG"
#define WIFI_CFG_PATH "/sdcard/WIFI/WIFI.CFG"

static const char *TAG = "config_store";

static void ensure_dir(const char *path)
{
    if (path) {
        mkdir(path, 0775);
    }
}

static inline void config_io_begin(void)
{
    eldra_platform_before_sd_io();
}

static inline void config_io_end(void)
{
    eldra_platform_after_sd_io();
}

void config_store_get_defaults(config_store_t *out)
{
    if (!out) return;
    out->auto_init_sd = true;
    out->auto_init_wifi = true;
    out->auto_init_rtc = true;
    out->auto_init_imu = true;
    out->auto_init_cloud = false;
    out->wifi_roam = true;
    out->wifi_ssid[0] = '\0';
    out->wifi_pass[0] = '\0';
    out->logs_to_sd = true;
    out->cloud_logs_console = false;
    strlcpy(out->cloud_base_url, "http://sn-llm-core.local:8030", sizeof(out->cloud_base_url));
    strlcpy(out->cloud_token, "the-old-ones", sizeof(out->cloud_token)); // configurable; change via console
    out->cloud_poll_interval_ms = 40000;
    out->cloud_state_interval_ms = 60000;
    out->cloud_log_interval_ms = 30000;
    // Default to true center; user-calibration will override and persist.
    out->eyes_center_x_offset = 0;
    out->eyes_center_y_offset = 0;
    out->display_center_x_offset = 0;
    out->display_center_y_offset = 0;
    out->glyph_offset_x = 0;
    out->glyph_offset_y = -170; // place near top outer-edge by default (center-based layout)
    out->glyph_scale = 2;
    out->sleep_lid_depth = 2;
    out->angry_lid_depth = 2;
    out->sleep_start_hour = 22;
    out->sleep_end_hour = 8;
    out->sleep_window_inactivity_ms = 300000;  // 5 minutes
    out->sleep_low_battery_pct = 10;           // 10%
    out->sleep_global_inactivity_ms = 1800000; // 30 minutes
    out->sleep_overfed_hold_ms = 120000;       // 2 minutes
    out->mood_log_interval_minutes = 5; // log meters every 5 minutes by default (0=off)
    out->affect_weight_happiness_pct = 100;
    out->affect_weight_satiety_pct = 100;
    out->affect_weight_energy_pct = 100;
    out->affect_weight_social_pct = 100;
    out->affect_weight_fear_pct = 100;
    out->angry_dizzy_count_threshold = 3;
    out->angry_dizzy_window_ms = 300000; // 5 minutes
    out->angry_override_min_ms = 120000; // 2 minutes
    out->angry_override_max_ms = 300000; // 5 minutes
    out->mood_happiness = -1;
    out->mood_hunger = -1;
    out->mood_energy = -1;
    out->mood_social = -1;
    out->mood_fear = -1;
    out->mood_eldritch_charge = -1;
    out->mood_state = -1;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void json_apply_bool(cJSON *obj, const char *key, bool *dst)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
    }
}

static void json_apply_string(cJSON *obj, const char *key, char *dst, size_t dstlen)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(dst, item->valuestring, dstlen);
    }
}

static void json_apply_int(cJSON *obj, const char *key, int *dst)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) {
        *dst = item->valueint;
    }
}

static esp_err_t save_defaults_to_disk(const config_store_t *cfg)
{
    return config_store_save(cfg);
}

esp_err_t config_store_factory_reset(void)
{
    config_store_t defaults = {0};
    config_store_get_defaults(&defaults);

    // Clear fallback mirrors first so reboot starts from true defaults.
    config_store_clear_eye_offsets_nvs();
    config_store_clear_mood_nvs();

    config_io_begin();
    if (unlink(CONFIG_PATH) != 0 && errno != ENOENT) {
        EL_LOGW(TAG, "Failed to remove %s (%s)", CONFIG_PATH, strerror(errno));
    }
    if (unlink(WIFI_CFG_PATH) != 0 && errno != ENOENT) {
        EL_LOGW(TAG, "Failed to remove %s (%s)", WIFI_CFG_PATH, strerror(errno));
    }
    config_io_end();

    return config_store_save(&defaults);
}

esp_err_t config_store_load(config_store_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    config_store_get_defaults(out);
    bool updated = false;

    if (!file_exists(CONFIG_PATH)) {
        EL_LOGW(TAG, "Config missing, writing defaults");
        esp_err_t r = save_defaults_to_disk(out);
        if (r != ESP_OK) {
            EL_LOGW(TAG, "Failed to write defaults (%s), using defaults in RAM", esp_err_to_name(r));
        }
        return ESP_OK;
    }

    config_io_begin();
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        config_io_end();
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 16 * 1024) {
        config_io_begin();
        fclose(f);
        config_io_end();
        config_io_end();
        return save_defaults_to_disk(out);
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        config_io_begin();
        fclose(f);
        config_io_end();
        config_io_end();
        return ESP_ERR_NO_MEM;
    }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    config_io_begin();
    fclose(f);
    config_io_end();
    config_io_end();

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        EL_LOGW(TAG, "Config parse failed, using defaults");
        esp_err_t r = save_defaults_to_disk(out);
        if (r != ESP_OK) {
            EL_LOGW(TAG, "Failed to write defaults (%s), using defaults in RAM", esp_err_to_name(r));
        }
        return ESP_OK;
    }

    cJSON *auto_init = cJSON_GetObjectItem(root, "auto_init");
    if (cJSON_IsObject(auto_init)) {
        if (!cJSON_HasObjectItem(auto_init, "sd")) updated = true;
        if (!cJSON_HasObjectItem(auto_init, "wifi")) updated = true;
        if (!cJSON_HasObjectItem(auto_init, "rtc")) updated = true;
        if (!cJSON_HasObjectItem(auto_init, "imu")) updated = true;
        if (!cJSON_HasObjectItem(auto_init, "cloud")) updated = true;
        json_apply_bool(auto_init, "sd", &out->auto_init_sd);
        json_apply_bool(auto_init, "wifi", &out->auto_init_wifi);
        json_apply_bool(auto_init, "rtc", &out->auto_init_rtc);
        json_apply_bool(auto_init, "imu", &out->auto_init_imu);
        json_apply_bool(auto_init, "cloud", &out->auto_init_cloud);
    } else {
        updated = true;
    }
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        if (!cJSON_HasObjectItem(wifi, "roam")) updated = true;
        if (!cJSON_HasObjectItem(wifi, "ssid")) updated = true;
        if (!cJSON_HasObjectItem(wifi, "pass")) updated = true;
        json_apply_bool(wifi, "roam", &out->wifi_roam);
        json_apply_string(wifi, "ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
        json_apply_string(wifi, "pass", out->wifi_pass, sizeof(out->wifi_pass));
    } else {
        updated = true;
    }
    cJSON *cloud = cJSON_GetObjectItem(root, "cloud");
    if (cJSON_IsObject(cloud)) {
        if (!cJSON_HasObjectItem(cloud, "base_url")) updated = true;
        if (!cJSON_HasObjectItem(cloud, "token")) updated = true;
        if (!cJSON_HasObjectItem(cloud, "poll_interval_ms")) updated = true;
        if (!cJSON_HasObjectItem(cloud, "state_interval_ms")) updated = true;
        if (!cJSON_HasObjectItem(cloud, "log_interval_ms")) updated = true;
        json_apply_string(cloud, "base_url", out->cloud_base_url, sizeof(out->cloud_base_url));
        json_apply_string(cloud, "token", out->cloud_token, sizeof(out->cloud_token));
        json_apply_int(cloud, "poll_interval_ms", &out->cloud_poll_interval_ms);
        json_apply_int(cloud, "state_interval_ms", &out->cloud_state_interval_ms);
        json_apply_int(cloud, "log_interval_ms", &out->cloud_log_interval_ms);
    } else {
        updated = true;
    }
    cJSON *logs = cJSON_GetObjectItem(root, "logs");
    if (cJSON_IsObject(logs)) {
        if (!cJSON_HasObjectItem(logs, "to_sd")) updated = true;
        json_apply_bool(logs, "to_sd", &out->logs_to_sd);
        if (!cJSON_HasObjectItem(logs, "cloud_console")) updated = true;
        json_apply_bool(logs, "cloud_console", &out->cloud_logs_console);
    } else {
        updated = true;
    }
    cJSON *mood = cJSON_GetObjectItem(root, "mood_state");
    if (cJSON_IsObject(mood)) {
        if (!cJSON_HasObjectItem(mood, "happiness")) updated = true;
        if (!cJSON_HasObjectItem(mood, "hunger")) updated = true;
        if (!cJSON_HasObjectItem(mood, "energy")) updated = true;
        if (!cJSON_HasObjectItem(mood, "social")) updated = true;
        if (!cJSON_HasObjectItem(mood, "fear")) updated = true;
        if (!cJSON_HasObjectItem(mood, "eldritch_charge")) updated = true;
        if (!cJSON_HasObjectItem(mood, "state")) updated = true;
        json_apply_int(mood, "happiness", &out->mood_happiness);
        json_apply_int(mood, "hunger", &out->mood_hunger);
        json_apply_int(mood, "energy", &out->mood_energy);
        json_apply_int(mood, "social", &out->mood_social);
        json_apply_int(mood, "fear", &out->mood_fear);
        json_apply_int(mood, "eldritch_charge", &out->mood_eldritch_charge);
        json_apply_int(mood, "state", &out->mood_state);
    } else {
        updated = true;
    }
    cJSON *eyes = cJSON_GetObjectItem(root, "eyes");
    if (cJSON_IsObject(eyes)) {
        if (!cJSON_HasObjectItem(eyes, "center_x_offset")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "center_y_offset")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "display_center_x_offset")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "display_center_y_offset")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "glyph_offset_x")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "glyph_offset_y")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "glyph_scale")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "sleep_lid_depth")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "angry_lid_depth")) updated = true;
        json_apply_int(eyes, "center_x_offset", &out->eyes_center_x_offset);
        json_apply_int(eyes, "center_y_offset", &out->eyes_center_y_offset);
        json_apply_int(eyes, "display_center_x_offset", &out->display_center_x_offset);
        json_apply_int(eyes, "display_center_y_offset", &out->display_center_y_offset);
        json_apply_int(eyes, "glyph_offset_x", &out->glyph_offset_x);
        json_apply_int(eyes, "glyph_offset_y", &out->glyph_offset_y);
        json_apply_int(eyes, "glyph_scale", &out->glyph_scale);
        json_apply_int(eyes, "sleep_lid_depth", &out->sleep_lid_depth);
        json_apply_int(eyes, "angry_lid_depth", &out->angry_lid_depth);
    } else {
        updated = true;
    }
    cJSON *sleep = cJSON_GetObjectItem(root, "sleep");
    if (cJSON_IsObject(sleep)) {
        if (!cJSON_HasObjectItem(sleep, "start_hour")) updated = true;
        if (!cJSON_HasObjectItem(sleep, "end_hour")) updated = true;
        if (!cJSON_HasObjectItem(sleep, "window_inactivity_ms")) updated = true;
        if (!cJSON_HasObjectItem(sleep, "low_battery_pct")) updated = true;
        if (!cJSON_HasObjectItem(sleep, "global_inactivity_ms")) updated = true;
        if (!cJSON_HasObjectItem(sleep, "overfed_hold_ms")) updated = true;
        json_apply_int(sleep, "start_hour", &out->sleep_start_hour);
        json_apply_int(sleep, "end_hour", &out->sleep_end_hour);
        json_apply_int(sleep, "window_inactivity_ms", &out->sleep_window_inactivity_ms);
        json_apply_int(sleep, "low_battery_pct", &out->sleep_low_battery_pct);
        json_apply_int(sleep, "global_inactivity_ms", &out->sleep_global_inactivity_ms);
        json_apply_int(sleep, "overfed_hold_ms", &out->sleep_overfed_hold_ms);
    } else {
        updated = true;
    }
    cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
    if (cJSON_IsObject(emotion)) {
        if (!cJSON_HasObjectItem(emotion, "mood_log_interval_minutes")) updated = true;
        if (!cJSON_HasObjectItem(emotion, "affect_weight_happiness_pct")) updated = true;
        if (!cJSON_HasObjectItem(emotion, "affect_weight_satiety_pct")) updated = true;
        if (!cJSON_HasObjectItem(emotion, "affect_weight_energy_pct")) updated = true;
        if (!cJSON_HasObjectItem(emotion, "affect_weight_social_pct")) updated = true;
        if (!cJSON_HasObjectItem(emotion, "affect_weight_fear_pct")) updated = true;
        if (!cJSON_HasObjectItem(emotion, "angry_dizzy_count_threshold")) updated = true;
        if (!cJSON_HasObjectItem(emotion, "angry_dizzy_window_ms")) updated = true;
        if (!cJSON_HasObjectItem(emotion, "angry_override_min_ms")) updated = true;
        if (!cJSON_HasObjectItem(emotion, "angry_override_max_ms")) updated = true;
        json_apply_int(emotion, "mood_log_interval_minutes", &out->mood_log_interval_minutes);
        json_apply_int(emotion, "affect_weight_happiness_pct", &out->affect_weight_happiness_pct);
        json_apply_int(emotion, "affect_weight_satiety_pct", &out->affect_weight_satiety_pct);
        json_apply_int(emotion, "affect_weight_energy_pct", &out->affect_weight_energy_pct);
        json_apply_int(emotion, "affect_weight_social_pct", &out->affect_weight_social_pct);
        json_apply_int(emotion, "affect_weight_fear_pct", &out->affect_weight_fear_pct);
        json_apply_int(emotion, "angry_dizzy_count_threshold", &out->angry_dizzy_count_threshold);
        json_apply_int(emotion, "angry_dizzy_window_ms", &out->angry_dizzy_window_ms);
        json_apply_int(emotion, "angry_override_min_ms", &out->angry_override_min_ms);
        json_apply_int(emotion, "angry_override_max_ms", &out->angry_override_max_ms);
    } else {
        updated = true;
    }
    cJSON_Delete(root);
    if (updated) {
        // Backfill newly added fields for future runs.
        (void)config_store_save(out);
    }
    return ESP_OK;
}

esp_err_t config_store_save(const config_store_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    ensure_dir(CONFIG_DIR);
    cJSON *root = cJSON_CreateObject();
    cJSON *auto_init = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "auto_init", auto_init);
    cJSON_AddBoolToObject(auto_init, "sd", cfg->auto_init_sd);
    cJSON_AddBoolToObject(auto_init, "wifi", cfg->auto_init_wifi);
    cJSON_AddBoolToObject(auto_init, "rtc", cfg->auto_init_rtc);
    cJSON_AddBoolToObject(auto_init, "imu", cfg->auto_init_imu);
    cJSON_AddBoolToObject(auto_init, "cloud", cfg->auto_init_cloud);

    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "wifi", wifi);
    cJSON_AddStringToObject(wifi, "ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(wifi, "pass", cfg->wifi_pass);
    cJSON_AddBoolToObject(wifi, "roam", cfg->wifi_roam);

    cJSON *cloud = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "cloud", cloud);
    cJSON_AddStringToObject(cloud, "base_url", cfg->cloud_base_url);
    cJSON_AddStringToObject(cloud, "token", cfg->cloud_token);
    cJSON_AddNumberToObject(cloud, "poll_interval_ms", cfg->cloud_poll_interval_ms);
    cJSON_AddNumberToObject(cloud, "state_interval_ms", cfg->cloud_state_interval_ms);
    cJSON_AddNumberToObject(cloud, "log_interval_ms", cfg->cloud_log_interval_ms);

    cJSON *logs = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "logs", logs);
    cJSON_AddBoolToObject(logs, "to_sd", cfg->logs_to_sd);
    cJSON_AddBoolToObject(logs, "cloud_console", cfg->cloud_logs_console);

    cJSON *eyes = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "eyes", eyes);
    cJSON_AddNumberToObject(eyes, "center_x_offset", cfg->eyes_center_x_offset);
    cJSON_AddNumberToObject(eyes, "center_y_offset", cfg->eyes_center_y_offset);
    cJSON_AddNumberToObject(eyes, "display_center_x_offset", cfg->display_center_x_offset);
    cJSON_AddNumberToObject(eyes, "display_center_y_offset", cfg->display_center_y_offset);
    cJSON_AddNumberToObject(eyes, "glyph_offset_x", cfg->glyph_offset_x);
    cJSON_AddNumberToObject(eyes, "glyph_offset_y", cfg->glyph_offset_y);
    cJSON_AddNumberToObject(eyes, "glyph_scale", cfg->glyph_scale);
    cJSON_AddNumberToObject(eyes, "sleep_lid_depth", cfg->sleep_lid_depth);
    cJSON_AddNumberToObject(eyes, "angry_lid_depth", cfg->angry_lid_depth);
    cJSON *sleep = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "sleep", sleep);
    cJSON_AddNumberToObject(sleep, "start_hour", cfg->sleep_start_hour);
    cJSON_AddNumberToObject(sleep, "end_hour", cfg->sleep_end_hour);
    cJSON_AddNumberToObject(sleep, "window_inactivity_ms", cfg->sleep_window_inactivity_ms);
    cJSON_AddNumberToObject(sleep, "low_battery_pct", cfg->sleep_low_battery_pct);
    cJSON_AddNumberToObject(sleep, "global_inactivity_ms", cfg->sleep_global_inactivity_ms);
    cJSON_AddNumberToObject(sleep, "overfed_hold_ms", cfg->sleep_overfed_hold_ms);
    cJSON *emotion = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "emotion", emotion);
    cJSON_AddNumberToObject(emotion, "mood_log_interval_minutes", cfg->mood_log_interval_minutes);
    cJSON_AddNumberToObject(emotion, "affect_weight_happiness_pct", cfg->affect_weight_happiness_pct);
    cJSON_AddNumberToObject(emotion, "affect_weight_satiety_pct", cfg->affect_weight_satiety_pct);
    cJSON_AddNumberToObject(emotion, "affect_weight_energy_pct", cfg->affect_weight_energy_pct);
    cJSON_AddNumberToObject(emotion, "affect_weight_social_pct", cfg->affect_weight_social_pct);
    cJSON_AddNumberToObject(emotion, "affect_weight_fear_pct", cfg->affect_weight_fear_pct);
    cJSON_AddNumberToObject(emotion, "angry_dizzy_count_threshold", cfg->angry_dizzy_count_threshold);
    cJSON_AddNumberToObject(emotion, "angry_dizzy_window_ms", cfg->angry_dizzy_window_ms);
    cJSON_AddNumberToObject(emotion, "angry_override_min_ms", cfg->angry_override_min_ms);
    cJSON_AddNumberToObject(emotion, "angry_override_max_ms", cfg->angry_override_max_ms);

    cJSON *mood = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "mood_state", mood);
    cJSON_AddNumberToObject(mood, "happiness", cfg->mood_happiness);
    cJSON_AddNumberToObject(mood, "hunger", cfg->mood_hunger);
    cJSON_AddNumberToObject(mood, "energy", cfg->mood_energy);
    cJSON_AddNumberToObject(mood, "social", cfg->mood_social);
    cJSON_AddNumberToObject(mood, "fear", cfg->mood_fear);
    cJSON_AddNumberToObject(mood, "eldritch_charge", cfg->mood_eldritch_charge);
    cJSON_AddNumberToObject(mood, "state", cfg->mood_state);

    char *printed = cJSON_PrintBuffered(root, 512, true);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }

    config_io_begin();
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        config_io_end();
        EL_LOGE(TAG, "Failed to open %s for write (%s)", CONFIG_PATH, strerror(errno));
        free(printed);
        return ESP_FAIL;
    }
    config_io_end();
    config_io_begin();
    fwrite(printed, 1, strlen(printed), f);
    config_io_end();
    config_io_begin();
    fclose(f);
    config_io_end();
    free(printed);
    return ESP_OK;
}

