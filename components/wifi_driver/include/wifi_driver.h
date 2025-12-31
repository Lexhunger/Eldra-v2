#pragma once
// Minimal Wi-Fi STA helper (independent of console/UI).

#include "esp_err.h"
#include <stdint.h>
#include "esp_wifi.h"

typedef enum {
    WIFI_STATUS_IDLE = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED,
} wifi_driver_status_t;

esp_err_t wifi_driver_init_sta(void);
esp_err_t wifi_driver_scan_and_log(uint16_t *ap_count_out);

// Connect to the strongest BSSID for the given SSID/password (2.4 GHz only).
esp_err_t wifi_driver_connect_best(const char *ssid, const char *password);

// Non-blocking connect: starts a task that scans and connects to strongest BSSID.
esp_err_t wifi_driver_connect_best_async(const char *ssid, const char *password);

// Enable/disable background roaming to the strongest BSSID of the configured SSID.
// Roaming runs only after a successful connect via wifi_driver_connect_best().
esp_err_t wifi_driver_set_roaming(bool enabled);

// Query current status and (optionally) current AP info.
esp_err_t wifi_driver_get_status(wifi_driver_status_t *status_out, wifi_ap_record_t *ap_out);

// Ping a host (hostname or IPv4 string). count: number of ICMP echo requests. timeout_ms per request.
esp_err_t wifi_driver_ping(const char *host, uint32_t count, uint32_t timeout_ms);

// Retrieve last saved credentials (from last connect attempt). Returns true if present.
bool wifi_driver_get_saved_credentials(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len);
