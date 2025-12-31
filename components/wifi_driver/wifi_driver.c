#include "wifi_driver.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/icmp.h"
#include "lwip/sockets.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"

static const char *TAG = "wifi_drv";
static bool wifi_initialized = false;

typedef struct {
    char ssid[33];
    int rssi;
    uint8_t channel;
} ssid_entry_t;

static int cmp_ssid_entry(const void *a, const void *b)
{
    int rssi_a = ((const ssid_entry_t *)a)->rssi;
    int rssi_b = ((const ssid_entry_t *)b)->rssi;
    return (rssi_b - rssi_a);
}

static char saved_ssid[33] = {0};
static char saved_pass[65] = {0};
static bool roaming_enabled = false;
static TaskHandle_t roam_task_handle = NULL;
static TaskHandle_t connect_task_handle = NULL;
static wifi_driver_status_t wifi_status = WIFI_STATUS_IDLE;
static wifi_ap_record_t current_ap = {0};
static esp_err_t last_err = ESP_OK;

static esp_err_t scan_best_for_ssid(const char *ssid, wifi_ap_record_t *best_out)
{
    if (!ssid || !best_out) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)ssid,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), TAG, "scan start failed");

    uint16_t ap_num = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_num), TAG, "get ap num failed");
    if (ap_num == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    wifi_ap_record_t *ap_records = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    if (ret != ESP_OK) {
        free(ap_records);
        return ret;
    }

    bool found = false;
    int best_idx = -1;
    for (int i = 0; i < ap_num; i++) {
        if (strcmp((char *)ap_records[i].ssid, ssid) != 0) {
            continue;
        }
        if (!found || ap_records[i].rssi > ap_records[best_idx].rssi) {
            best_idx = i;
            found = true;
        }
    }
    if (found) {
        *best_out = ap_records[best_idx];
        ESP_LOGI(TAG, "Best for SSID \"%s\": RSSI=%d CH=%d BSSID=%02X:%02X:%02X:%02X:%02X:%02X",
                 ssid, best_out->rssi, best_out->primary,
                 best_out->bssid[0], best_out->bssid[1], best_out->bssid[2],
                 best_out->bssid[3], best_out->bssid[4], best_out->bssid[5]);
    } else {
        ret = ESP_ERR_NOT_FOUND;
    }
    free(ap_records);
    return ret;
}

static void roam_task(void *arg)
{
    const int roam_interval_ms = 30000;
    const int roam_threshold_db = 10; // switch if better AP is at least 10 dB stronger

    while (roaming_enabled) {
        vTaskDelay(pdMS_TO_TICKS(roam_interval_ms));
        if (!roaming_enabled || saved_ssid[0] == '\0') {
            continue;
        }

        wifi_ap_record_t current = {0};
        if (esp_wifi_sta_get_ap_info(&current) != ESP_OK) {
            continue;
        }

        wifi_ap_record_t best = {0};
        if (scan_best_for_ssid(saved_ssid, &best) != ESP_OK) {
            continue;
        }

        if (memcmp(best.bssid, current.bssid, 6) != 0 && best.rssi > current.rssi + roam_threshold_db) {
            ESP_LOGI(TAG, "Roaming to stronger BSSID (old RSSI %d -> new RSSI %d)", current.rssi, best.rssi);
            wifi_config_t cfg = {0};
            strlcpy((char *)cfg.sta.ssid, saved_ssid, sizeof(cfg.sta.ssid));
            strlcpy((char *)cfg.sta.password, saved_pass, sizeof(cfg.sta.password));
            memcpy(cfg.sta.bssid, best.bssid, 6);
            cfg.sta.bssid_set = true;
            cfg.sta.channel = best.primary;
            cfg.sta.scan_method = WIFI_FAST_SCAN;
            esp_wifi_disconnect();
            esp_wifi_set_config(WIFI_IF_STA, &cfg);
            esp_wifi_connect();
        }
    }
    roam_task_handle = NULL;
    vTaskDelete(NULL);
}

static void connect_task(void *arg)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    // Copy credentials from saved_* to avoid races.
    strlcpy(ssid, saved_ssid, sizeof(ssid));
    strlcpy(pass, saved_pass, sizeof(pass));

    wifi_status = WIFI_STATUS_CONNECTING;
    wifi_ap_record_t best = {0};
    esp_err_t ret = scan_best_for_ssid(ssid, &best);
    if (ret == ESP_OK) {
        wifi_config_t cfg = {0};
        strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
        memcpy(cfg.sta.bssid, best.bssid, 6);
        cfg.sta.bssid_set = true;
        cfg.sta.channel = best.primary;
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        cfg.sta.scan_method = WIFI_FAST_SCAN;

        ret = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (ret == ESP_OK) {
            ret = esp_wifi_connect();
        }
    }

    last_err = ret;
    if (ret == ESP_OK) {
        // Best effort to fetch current AP info after connect
        vTaskDelay(pdMS_TO_TICKS(200));
        if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK) {
            wifi_status = WIFI_STATUS_CONNECTED;
        } else {
            wifi_status = WIFI_STATUS_FAILED;
        }
    } else {
        wifi_status = WIFI_STATUS_FAILED;
    }

    connect_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_driver_init_sta(void)
{
    if (wifi_initialized) {
        return ESP_OK;
    }
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs init failed");

    esp_err_t err;
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "netif init failed");
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "event loop init failed");
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    // Quiet verbose Wi-Fi/PHY logs so they don't drown the console prompt.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("phy", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);

    wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi STA initialized");
    return ESP_OK;
}

esp_err_t wifi_driver_scan_and_log(uint16_t *ap_count_out)
{
    ESP_RETURN_ON_ERROR(wifi_driver_init_sta(), TAG, "wifi init failed");

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), TAG, "scan start failed");

    uint16_t ap_num = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_num), TAG, "get ap num failed");
    if (ap_count_out) {
        *ap_count_out = ap_num;
    }

    ESP_LOGI(TAG, "Found %u APs", ap_num);
    if (ap_num == 0) {
        return ESP_OK;
    }

    wifi_ap_record_t *ap_records = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    if (ret == ESP_OK) {
        ssid_entry_t *entries = calloc(ap_num, sizeof(ssid_entry_t));
        int unique = 0;

        for (int i = 0; i < ap_num; i++) {
            if (ap_records[i].ssid[0] == '\0') {
                continue; // skip empty SSIDs
            }
            // look for existing
            int idx = -1;
            for (int j = 0; j < unique; j++) {
                if (strcmp(entries[j].ssid, (char *)ap_records[i].ssid) == 0) {
                    idx = j;
                    break;
                }
            }
            if (idx == -1) {
                idx = unique++;
                strlcpy(entries[idx].ssid, (char *)ap_records[i].ssid, sizeof(entries[idx].ssid));
                entries[idx].rssi = ap_records[i].rssi;
                entries[idx].channel = ap_records[i].primary;
            } else if (ap_records[i].rssi > entries[idx].rssi) {
                // keep the strongest
                entries[idx].rssi = ap_records[i].rssi;
                entries[idx].channel = ap_records[i].primary;
            }
        }

        // sort by RSSI descending
        if (unique > 1) {
            qsort(entries, unique, sizeof(ssid_entry_t), cmp_ssid_entry);
        }

        ESP_LOGI(TAG, "Found %u APs (%d unique SSIDs)", ap_num, unique);
        for (int i = 0; i < unique; i++) {
            ESP_LOGI(TAG, "%2d: SSID=\"%s\" RSSI=%d CH=%d", i + 1, entries[i].ssid, entries[i].rssi, entries[i].channel);
        }
        free(entries);
    }
    free(ap_records);
    return ret;
}

esp_err_t wifi_driver_connect_best(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(wifi_driver_init_sta(), TAG, "wifi init failed");

    wifi_ap_record_t best = {0};
    esp_err_t ret = scan_best_for_ssid(ssid, &best);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    memcpy(cfg.sta.bssid, best.bssid, 6);
    cfg.sta.bssid_set = true;
    cfg.sta.channel = best.primary;
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.scan_method = WIFI_FAST_SCAN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect failed");

    strlcpy(saved_ssid, ssid, sizeof(saved_ssid));
    strlcpy(saved_pass, password, sizeof(saved_pass));
    wifi_status = WIFI_STATUS_CONNECTED; // optimistic; caller can query/get_ap_info for confirmation
    return ESP_OK;
}

esp_err_t wifi_driver_set_roaming(bool enabled)
{
    roaming_enabled = enabled;
    if (enabled && !roam_task_handle && saved_ssid[0] != '\0') {
    BaseType_t ok = xTaskCreatePinnedToCore(roam_task, "wifi_roam", 4096, NULL, 3, &roam_task_handle, 1);
    if (ok != pdPASS) {
        roam_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    }
    // task exits on its own when roaming_enabled becomes false
    return ESP_OK;
}

esp_err_t wifi_driver_connect_best_async(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(wifi_driver_init_sta(), TAG, "wifi init failed");

    strlcpy(saved_ssid, ssid, sizeof(saved_ssid));
    strlcpy(saved_pass, password, sizeof(saved_pass));
    wifi_status = WIFI_STATUS_CONNECTING;
    last_err = ESP_OK;

    if (connect_task_handle) {
        // already running
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(connect_task, "wifi_connect", 4096, NULL, 3, &connect_task_handle, 1);
    if (ok != pdPASS) {
        connect_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_driver_get_status(wifi_driver_status_t *status_out, wifi_ap_record_t *ap_out)
{
    if (status_out) {
        *status_out = wifi_status;
    }
    if (ap_out && wifi_status == WIFI_STATUS_CONNECTED) {
        if (esp_wifi_sta_get_ap_info(ap_out) != ESP_OK) {
            memset(ap_out, 0, sizeof(*ap_out));
        }
    }
    return last_err;
}

esp_err_t wifi_driver_ping(const char *host, uint32_t count, uint32_t timeout_ms)
{
    if (!host || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(wifi_driver_init_sta(), TAG, "wifi init failed");

    struct sockaddr_in target = {
        .sin_family = AF_INET,
    };

    if (inet_aton(host, &target.sin_addr) == 0) {
        // Resolve hostname
        struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_RAW,
            .ai_protocol = IP_PROTO_ICMP,
        };
        struct addrinfo *res = NULL;
        int err = getaddrinfo(host, NULL, &hints, &res);
        if (err != 0 || !res) {
            if (res) freeaddrinfo(res);
            return ESP_ERR_NOT_FOUND;
        }
        target = *(struct sockaddr_in *)res->ai_addr;
        freeaddrinfo(res);
    }

    int sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (sock < 0) {
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint16_t id = (uint16_t)esp_random();
    uint16_t seq = 0;
    uint32_t sent = 0;
    uint32_t recv = 0;
    uint32_t total_time = 0;

    const size_t data_len = 32;
    uint8_t buf[sizeof(struct icmp_echo_hdr) + data_len];

    for (uint32_t i = 0; i < count; i++) {
        struct icmp_echo_hdr *hdr = (struct icmp_echo_hdr *)buf;
        hdr->type = ICMP_ECHO;
        hdr->code = 0;
        hdr->chksum = 0;
        hdr->id = lwip_htons(id);
        hdr->seqno = lwip_htons(seq++);
        memset(buf + sizeof(*hdr), 0xAA, data_len);
        hdr->chksum = inet_chksum(buf, sizeof(buf));

        int err = sendto(sock, buf, sizeof(buf), 0, (struct sockaddr *)&target, sizeof(target));
        if (err < 0) {
            continue;
        }
        sent++;

        uint32_t start = xTaskGetTickCount();
        struct sockaddr_in from = {0};
        socklen_t fromlen = sizeof(from);
        uint8_t rbuf[64] = {0};
        err = recvfrom(sock, rbuf, sizeof(rbuf), 0, (struct sockaddr *)&from, &fromlen);
        if (err >= (int)sizeof(struct ip_hdr) + (int)sizeof(struct icmp_echo_hdr)) {
            struct ip_hdr *iphdr = (struct ip_hdr *)rbuf;
            struct icmp_echo_hdr *icmphdr = (struct icmp_echo_hdr *)(rbuf + (IPH_HL(iphdr) * 4));
            if (icmphdr->type == ICMP_ER && icmphdr->id == lwip_htons(id)) {
                recv++;
                uint32_t dur = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
                total_time += dur;
            }
        }
    }

    close(sock);
    printf("Ping %s: sent=%" PRIu32 " recv=%" PRIu32 " avg_time=%" PRIu32 "ms\n",
           host, sent, recv, (recv > 0) ? total_time / recv : 0);
    return (recv > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_driver_get_saved_credentials(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    if (!ssid_out || !pass_out || ssid_len == 0 || pass_len == 0) {
        return false;
    }
    if (saved_ssid[0] == '\0') {
        return false;
    }
    strlcpy(ssid_out, saved_ssid, ssid_len);
    strlcpy(pass_out, saved_pass, pass_len);
    return true;
}
