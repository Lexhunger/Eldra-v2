#include "eldra_wifi_provision.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "eldra_sd.h"
#include "eldra_logging.h"

#ifndef ELDRA_WIFI_PROV_SSID
#define ELDRA_WIFI_PROV_SSID ""
#endif

#ifndef ELDRA_WIFI_PROV_PASS
#define ELDRA_WIFI_PROV_PASS ""
#endif

#ifdef ELDRA_WIFI_PROV_WRITE
#define ELDRA_WIFI_PROV_WRITE_ENABLED 1
#else
#define ELDRA_WIFI_PROV_WRITE_ENABLED 0
#endif

#define TAG "wifi_prov"
#define SD_WAIT_MS 5000
#define SD_POLL_INTERVAL_MS 250
#define WIFI_CONNECT_TIMEOUT_MS 30000

static EventGroupHandle_t s_wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_DONE_BIT = BIT1;
static char s_config_path[64] = ELDRA_WIFI_CONFIG_PATH;
static TaskHandle_t s_connect_task = NULL;
static esp_err_t s_connect_result = ESP_FAIL;
static bool s_wifi_inited = false;
static eldra_wifi_credentials_t s_creds = {0};
static bool s_wifi_started = false;

static esp_err_t ensure_nvs_ready(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void log_sd_root_listing(void)
{
    eldra_sd_log_root(20);
}

static esp_err_t ensure_wifi_ready_for_scan(void)
{
    esp_err_t err = ensure_nvs_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_wifi_inited) {
        if (esp_netif_init() != ESP_OK) {
            return ESP_FAIL;
        }
        esp_err_t evt_ret = esp_event_loop_create_default();
        if (evt_ret != ESP_OK && evt_ret != ESP_ERR_INVALID_STATE) {
            return evt_ret;
        }
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) {
            return ESP_FAIL;
        }
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        s_wifi_inited = true;
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_STA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (!s_wifi_started) {
        esp_err_t start_ret = esp_wifi_start();
        if (start_ret != ESP_OK && start_ret != ESP_ERR_WIFI_CONN) {
            return start_ret;
        }
        s_wifi_started = true;
    }
    return ESP_OK;
}

static void reset_config_path(void)
{
    snprintf(s_config_path, sizeof(s_config_path), "%s", ELDRA_WIFI_CONFIG_PATH);
}

static bool find_config_path_on_sd(void)
{
    DIR *dir = opendir("/sdcard");
    if (!dir) {
        return false;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' || ent->d_name[0] == '\0') {
            continue;
        }
        if (strncasecmp(ent->d_name, "wifi", 4) == 0) {
            const char *dot = strrchr(ent->d_name, '.');
            if (dot && (strncasecmp(dot, ".conf", 5) == 0 || strncasecmp(dot, ".con", 4) == 0)) {
                strncpy(s_config_path, "/sdcard/", sizeof(s_config_path) - 1);
                strncat(s_config_path, ent->d_name, sizeof(s_config_path) - strlen(s_config_path) - 1);
                closedir(dir);
                EL_LOGI(TAG, "Using config path: %s", s_config_path);
                return true;
            }
        }
    }
    closedir(dir);
    return false;
}

static void log_available_networks(void)
{
    if (ensure_wifi_ready_for_scan() != ESP_OK) {
        EL_LOGW(TAG, "Wi-Fi driver not ready for scan");
        return;
    }
    wifi_scan_config_t scan_cfg = {0};
    EL_LOGI(TAG, "Scanning for Wi-Fi networks (blocking)...");
    esp_err_t scan_ret = esp_wifi_scan_start(&scan_cfg, true);
    if (scan_ret != ESP_OK) {
        EL_LOGW(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(scan_ret));
        return;
    }
    uint16_t ap_count = 0;
    esp_err_t num_ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (num_ret != ESP_OK) {
        EL_LOGW(TAG, "Wi-Fi scan get num failed: %s", esp_err_to_name(num_ret));
        return;
    }
    if (ap_count == 0) {
        EL_LOGI(TAG, "No Wi-Fi networks found");
        return;
    }
    if (ap_count > 10) {
        ap_count = 10;
    }
    wifi_ap_record_t records[10];
    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK) {
        EL_LOGW(TAG, "Failed to get scan records");
        return;
    }
    for (uint16_t i = 0; i < ap_count; ++i) {
        char ssid[33] = {0};
        memcpy(ssid, records[i].ssid, sizeof(records[i].ssid));
        EL_LOGI(TAG, "AP[%u]: %s RSSI=%d", (unsigned)i, ssid, records[i].rssi);
    }
}

bool eldra_wifi_provision_file_exists(void)
{
    struct stat st;
    return stat(s_config_path, &st) == 0;
}

static esp_err_t write_creds_if_missing(void)
{
    if (!ELDRA_WIFI_PROV_WRITE_ENABLED) {
        return ESP_OK;
    }

    if (eldra_wifi_provision_file_exists()) {
        EL_LOGI(TAG, "Wi-Fi config already present; not writing");
        return ESP_OK;
    }

    const char *ssid = ELDRA_WIFI_PROV_SSID;
    const char *pass = ELDRA_WIFI_PROV_PASS;
    if (!ssid || !pass || ssid[0] == '\0' || pass[0] == '\0') {
        EL_LOGE(TAG, "ELDRA_WIFI_PROV_SSID/PASS not provided; cannot write file");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(ELDRA_WIFI_CONFIG_PATH, "w");
    if (!f) {
        EL_LOGE(TAG, "Failed to open %s for writing", ELDRA_WIFI_CONFIG_PATH);
        return ESP_FAIL;
    }
    fprintf(f, "ssid=%s\n", ssid);
    fprintf(f, "pass=%s\n", pass);
    fclose(f);
    EL_LOGI(TAG, "Wi-Fi creds written to %s", ELDRA_WIFI_CONFIG_PATH);
    return ESP_OK;
}

esp_err_t eldra_wifi_provision_init(void)
{
    reset_config_path();
    EL_LOGI(TAG, "Provision init: waiting for SD");
    if (eldra_sd_wait_for_mount(SD_WAIT_MS, SD_POLL_INTERVAL_MS) != ESP_OK) {
        EL_LOGE(TAG, "SD card not mounted; wifi provision skipped");
        return ESP_ERR_INVALID_STATE;
    }
    log_sd_root_listing();
    find_config_path_on_sd();
    EL_LOGI(TAG, "Wi-Fi config file %s %s", s_config_path,
             eldra_wifi_provision_file_exists() ? "found" : "missing");
    return write_creds_if_missing();
}

esp_err_t eldra_wifi_provision_load(eldra_wifi_credentials_t *out_creds)
{
    if (!out_creds) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_creds, 0, sizeof(*out_creds));

    if (eldra_sd_wait_for_mount(SD_WAIT_MS, SD_POLL_INTERVAL_MS) != ESP_OK) {
        EL_LOGE(TAG, "SD card not mounted; cannot load Wi-Fi creds");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(s_config_path, "r");
    if (!f) {
        if (find_config_path_on_sd()) {
            f = fopen(s_config_path, "r");
        }
    }
    if (!f) {
        EL_LOGE(TAG, "Wi-Fi config file not found at %s", s_config_path);
        return ESP_ERR_NOT_FOUND;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *newline = strchr(line, '\n');
        if (newline) {
            *newline = '\0';
        }
        if (strncmp(line, "ssid=", 5) == 0) {
            strncpy(out_creds->ssid, line + 5, sizeof(out_creds->ssid) - 1);
        } else if (strncmp(line, "pass=", 5) == 0) {
            strncpy(out_creds->pass, line + 5, sizeof(out_creds->pass) - 1);
        }
    }
    fclose(f);

    if (out_creds->ssid[0] == '\0' || out_creds->pass[0] == '\0') {
        EL_LOGE(TAG, "Wi-Fi config missing ssid/pass");
        return ESP_ERR_INVALID_STATE;
    }

    EL_LOGI(TAG, "Wi-Fi creds loaded (ssid=%s)", out_creds->ssid);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                EL_LOGI(TAG, "WIFI_EVENT_STA_START");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED: {
                wifi_event_sta_connected_t *c = (wifi_event_sta_connected_t *)event_data;
                EL_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED: ssid=\"%.*s\" bssid=%02x:%02x:%02x:%02x:%02x:%02x chan=%u",
                         c ? c->ssid_len : 0, c ? (char *)c->ssid : "",
                         c ? c->bssid[0] : 0, c ? c->bssid[1] : 0, c ? c->bssid[2] : 0,
                         c ? c->bssid[3] : 0, c ? c->bssid[4] : 0, c ? c->bssid[5] : 0,
                         c ? c->channel : 0);
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
                EL_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED: reason=%d", disc ? disc->reason : -1);
                esp_wifi_connect();
                break;
            }
            default:
                EL_LOGI(TAG, "WIFI_EVENT id=%" PRIi32, event_id);
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        EL_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

static void wifi_connect_task(void *arg)
{
    (void)arg;
    s_connect_result = ESP_ERR_TIMEOUT;
    EL_LOGI(TAG, "Wi-Fi connect task start");
    ESP_EARLY_LOGI(TAG, "Wi-Fi connect task start (early)");
    // Ensure console verbosity is high enough for troubleshooting this session.
    log_set_console_level(LOG_LEVEL_INFO);
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("wifi", ESP_LOG_VERBOSE); // get verbose Wi-Fi driver logs
    esp_log_level_set(TAG, ESP_LOG_INFO);
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DONE_BIT);
    }

    esp_err_t err = ensure_nvs_ready();
    if (err != ESP_OK) {
        EL_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        ESP_EARLY_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        goto done;
    }
    EL_LOGI(TAG, "NVS ready");
    ESP_EARLY_LOGI(TAG, "NVS ready");

    if (!s_wifi_inited) {
        if (ensure_wifi_ready_for_scan() != ESP_OK) {
            EL_LOGE(TAG, "Wi-Fi init failed");
            ESP_EARLY_LOGE(TAG, "Wi-Fi init failed");
            goto done;
        }
        EL_LOGI(TAG, "Wi-Fi driver inited");
        ESP_EARLY_LOGI(TAG, "Wi-Fi driver inited");
    }

    esp_err_t h1 = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_err_t h2 = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    EL_LOGI(TAG, "Event handlers registered");
    if (h1 != ESP_OK) {
        EL_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(h1));
    }
    if (h2 != ESP_OK) {
        EL_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(h2));
    }
    ESP_EARLY_LOGI(TAG, "Event handlers registered");

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_creds.ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_creds.pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    log_available_networks(); // blocking scan for visibility
    EL_LOGI(TAG, "Connecting to SSID: %s", s_creds.ssid);
    ESP_EARLY_LOGI(TAG, "Connecting to SSID: %s", s_creds.ssid);
    EL_LOGI(TAG, "esp_wifi_set_mode STA");
    esp_err_t set_mode_ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (set_mode_ret != ESP_OK) {
        EL_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(set_mode_ret));
        ESP_EARLY_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(set_mode_ret));
        goto done;
    }
    EL_LOGI(TAG, "esp_wifi_set_config ssid_len=%u", (unsigned)strlen((char *)wifi_config.sta.ssid));
    esp_err_t cfg_ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (cfg_ret != ESP_OK) {
        EL_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(cfg_ret));
        ESP_EARLY_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(cfg_ret));
        goto done;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        EL_LOGI(TAG, "STA MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_EARLY_LOGI(TAG, "STA MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    EL_LOGI(TAG, "esp_wifi_start (no stop)");
    ESP_EARLY_LOGI(TAG, "esp_wifi_start (no stop)");
    esp_err_t start_ret = esp_wifi_start();
    if (start_ret != ESP_OK && start_ret != ESP_ERR_WIFI_CONN) {
        EL_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(start_ret));
        ESP_EARLY_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(start_ret));
        goto done;
    }
    s_wifi_started = true;
    // Scan once before attempting to connect so we know what is visible.
    log_available_networks();

    EL_LOGI(TAG, "esp_wifi_connect (manual trigger)");
    ESP_EARLY_LOGI(TAG, "esp_wifi_connect (manual trigger)");
    esp_err_t conn_ret = esp_wifi_connect();
    if (conn_ret != ESP_OK) {
        EL_LOGE(TAG, "esp_wifi_connect returned error: %s", esp_err_to_name(conn_ret));
        ESP_EARLY_LOGE(TAG, "esp_wifi_connect returned error: %s", esp_err_to_name(conn_ret));
        goto done;
    }
    ESP_EARLY_LOGI(TAG, "Waiting for IP/connected...");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        EL_LOGI(TAG, "Wi-Fi connected");
        ESP_EARLY_LOGI(TAG, "Wi-Fi connected");
        s_connect_result = ESP_OK;
    } else {
        EL_LOGE(TAG, "Wi-Fi connect timeout");
        ESP_EARLY_LOGE(TAG, "Wi-Fi connect timeout");
        s_connect_result = ESP_ERR_TIMEOUT;
    }

done:
    if (s_wifi_event_group) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_DONE_BIT);
    }
    EL_LOGI(TAG, "Wi-Fi connect task exit");
    ESP_EARLY_LOGI(TAG, "Wi-Fi connect task exit");
    s_connect_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t eldra_wifi_provision_connect(void)
{
    ESP_EARLY_LOGI(TAG, "Connect API entry");
    if (s_connect_task) {
        EL_LOGW(TAG, "Wi-Fi connect already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    EL_LOGI(TAG, "Wi-Fi connect: loading credentials from SD");
    memset(&s_creds, 0, sizeof(s_creds));
    esp_err_t err = eldra_wifi_provision_load(&s_creds);
    if (err != ESP_OK) {
        EL_LOGE(TAG, "Cannot load Wi-Fi creds: %s", esp_err_to_name(err));
        return err;
    }
    EL_LOGI(TAG, "Loaded Wi-Fi creds: ssid=\"%s\" (len=%u)", s_creds.ssid, (unsigned)strlen(s_creds.ssid));

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            EL_LOGE(TAG, "Failed to create Wi-Fi event group");
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DONE_BIT);

    BaseType_t created = xTaskCreate(wifi_connect_task, "wifi_connect_task", 4096, NULL, 5, &s_connect_task);
    if (created != pdPASS) {
        s_connect_task = NULL;
        EL_LOGE(TAG, "Failed to create wifi_connect_task");
        return ESP_ERR_NO_MEM;
    }
    EL_LOGI(TAG, "wifi_connect_task created");

    return ESP_OK;
}

esp_err_t eldra_wifi_provision_wait(uint32_t timeout_ms)
{
    if (!s_wifi_event_group) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_DONE_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if (!(bits & WIFI_DONE_BIT)) {
        EL_LOGE(TAG, "Wi-Fi wait timeout after %u ms", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    if (s_connect_result == ESP_OK) {
        EL_LOGI(TAG, "Wi-Fi wait: connected");
    } else {
        EL_LOGE(TAG, "Wi-Fi wait: failed result=%s", esp_err_to_name(s_connect_result));
    }
    return s_connect_result;
}

void eldra_wifi_provision_scan_and_log(void)
{
    if (!s_wifi_inited) {
        EL_LOGI(TAG, "Wi-Fi driver not inited; cannot scan");
        return;
    }
    EL_LOGI(TAG, "Manual scan requested");
    log_available_networks();
}

esp_err_t eldra_wifi_provision_scan_collect(wifi_ap_record_t *records, size_t max_records, size_t *out_count)
{
    if (!records || max_records == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ready = ensure_wifi_ready_for_scan();
    if (ready != ESP_OK) {
        return ready;
    }
    wifi_scan_config_t cfg = {0};
    esp_err_t scan_ret = esp_wifi_scan_start(&cfg, true);
    if (scan_ret != ESP_OK) {
        return scan_ret;
    }
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) {
        return ESP_FAIL;
    }
    if (num > max_records) {
        num = (uint16_t)max_records;
    }
    if (esp_wifi_scan_get_ap_records(&num, records) != ESP_OK) {
        return ESP_FAIL;
    }
    *out_count = num;
    return ESP_OK;
}
