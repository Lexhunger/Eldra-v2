#include "config_store.h"

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

static const char *TAG = "config_store";

static void ensure_dir(const char *path)
{
    if (path) {
        mkdir(path, 0775);
    }
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
    strlcpy(out->cloud_base_url, "http://sn-llm-core.local:8030", sizeof(out->cloud_base_url));
    strlcpy(out->cloud_token, "the-old-ones", sizeof(out->cloud_token)); // configurable; change via console
    // Default display offset tuned for the round ST7701S panel from the demo build.
    out->eyes_center_x_offset = 20;
    out->eyes_center_y_offset = 0;
    out->display_center_x_offset = 0;
    out->display_center_y_offset = 0;
    out->sleep_start_hour = 22;
    out->sleep_end_hour = 8;
    out->mood_log_interval_minutes = 20; // log meters every 20 minutes by default
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

esp_err_t config_store_load(config_store_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    config_store_get_defaults(out);
    bool updated = false;

    if (!file_exists(CONFIG_PATH)) {
        ESP_LOGW(TAG, "Config missing, writing defaults");
        esp_err_t r = save_defaults_to_disk(out);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "Failed to write defaults (%s), using defaults in RAM", esp_err_to_name(r));
        }
        return ESP_OK;
    }

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 16 * 1024) {
        fclose(f);
        return save_defaults_to_disk(out);
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Config parse failed, using defaults");
        esp_err_t r = save_defaults_to_disk(out);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "Failed to write defaults (%s), using defaults in RAM", esp_err_to_name(r));
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
        json_apply_string(cloud, "base_url", out->cloud_base_url, sizeof(out->cloud_base_url));
        json_apply_string(cloud, "token", out->cloud_token, sizeof(out->cloud_token));
    } else {
        updated = true;
    }
    cJSON *logs = cJSON_GetObjectItem(root, "logs");
    if (cJSON_IsObject(logs)) {
        if (!cJSON_HasObjectItem(logs, "to_sd")) updated = true;
        json_apply_bool(logs, "to_sd", &out->logs_to_sd);
    } else {
        updated = true;
    }
    cJSON *eyes = cJSON_GetObjectItem(root, "eyes");
    if (cJSON_IsObject(eyes)) {
        if (!cJSON_HasObjectItem(eyes, "center_x_offset")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "center_y_offset")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "display_center_x_offset")) updated = true;
        if (!cJSON_HasObjectItem(eyes, "display_center_y_offset")) updated = true;
        json_apply_int(eyes, "center_x_offset", &out->eyes_center_x_offset);
        json_apply_int(eyes, "center_y_offset", &out->eyes_center_y_offset);
        json_apply_int(eyes, "display_center_x_offset", &out->display_center_x_offset);
        json_apply_int(eyes, "display_center_y_offset", &out->display_center_y_offset);
    } else {
        updated = true;
    }
    cJSON *sleep = cJSON_GetObjectItem(root, "sleep");
    if (cJSON_IsObject(sleep)) {
        if (!cJSON_HasObjectItem(sleep, "start_hour")) updated = true;
        if (!cJSON_HasObjectItem(sleep, "end_hour")) updated = true;
        json_apply_int(sleep, "start_hour", &out->sleep_start_hour);
        json_apply_int(sleep, "end_hour", &out->sleep_end_hour);
    } else {
        updated = true;
    }
    cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
    if (cJSON_IsObject(emotion)) {
        if (!cJSON_HasObjectItem(emotion, "mood_log_interval_minutes")) updated = true;
        json_apply_int(emotion, "mood_log_interval_minutes", &out->mood_log_interval_minutes);
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

    cJSON *logs = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "logs", logs);
    cJSON_AddBoolToObject(logs, "to_sd", cfg->logs_to_sd);

    cJSON *eyes = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "eyes", eyes);
    cJSON_AddNumberToObject(eyes, "center_x_offset", cfg->eyes_center_x_offset);
    cJSON_AddNumberToObject(eyes, "center_y_offset", cfg->eyes_center_y_offset);
    cJSON_AddNumberToObject(eyes, "display_center_x_offset", cfg->display_center_x_offset);
    cJSON_AddNumberToObject(eyes, "display_center_y_offset", cfg->display_center_y_offset);
    cJSON *sleep = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "sleep", sleep);
    cJSON_AddNumberToObject(sleep, "start_hour", cfg->sleep_start_hour);
    cJSON_AddNumberToObject(sleep, "end_hour", cfg->sleep_end_hour);
    cJSON *emotion = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "emotion", emotion);
    cJSON_AddNumberToObject(emotion, "mood_log_interval_minutes", cfg->mood_log_interval_minutes);

    char *printed = cJSON_PrintBuffered(root, 512, true);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for write (%s)", CONFIG_PATH, strerror(errno));
        free(printed);
        return ESP_FAIL;
    }
    fwrite(printed, 1, strlen(printed), f);
    fclose(f);
    free(printed);
    return ESP_OK;
}
