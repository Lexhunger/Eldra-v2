#include "console_wifi.h"
#include "eldra_logging.h"

#include "esp_console.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "wifi_driver.h"

static const char *TAG = "console_wifi";
typedef struct {
    char host[96];
    uint32_t count;
    uint32_t timeout_ms;
} wifi_ping_args_t;

static void wifi_ping_task(void *arg)
{
    wifi_ping_args_t *a = (wifi_ping_args_t *)arg;
    if (!a) {
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ret = wifi_driver_ping(a->host, a->count, a->timeout_ms);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        printf("Ping not supported in this build.\n");
    } else if (ret != ESP_OK) {
        printf("Ping failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("Ping OK\n");
    }

    free(a);
    vTaskDelete(NULL);
}

static void wifi_scan_task(void *arg)
{
    uint16_t count = 0;
    esp_err_t ret = wifi_driver_scan_and_log(&count);
    if (ret == ESP_OK) {
        printf("WiFi scan complete: %u APs found\n", count);
    } else {
        printf("WiFi scan failed: %s\n", esp_err_to_name(ret));
    }
    vTaskDelete(NULL);
}

static int cmd_wifi_init(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t ret = wifi_driver_init_sta();
    if (ret != ESP_OK) {
        printf("WiFi init failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("WiFi init OK\n");
    }
    return 0;
}

static int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    // Run scan in its own task to avoid blocking console REPL.
    BaseType_t ok = xTaskCreatePinnedToCore(wifi_scan_task, "wifi_scan", 4096, NULL, 3, NULL, 0);
    if (ok != pdPASS) {
        printf("Failed to start scan task\n");
    }
    return 0;
}

static int cmd_wifi_connect(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: wifi_connect <ssid> <pass>\n");
        return 0;
    }
    esp_err_t ret = wifi_driver_connect_best_async(argv[1], argv[2]);
    if (ret != ESP_OK) {
        printf("WiFi connect failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("WiFi connect task started for \"%s\"\n", argv[1]);
    }
    return 0;
}

static int cmd_wifi_roam(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: wifi_roam <on|off>\n");
        return 0;
    }
    bool enable = (strcmp(argv[1], "on") == 0);
    esp_err_t ret = wifi_driver_set_roaming(enable);
    if (ret != ESP_OK) {
        printf("Set roaming failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("Roaming %s\n", enable ? "enabled" : "disabled");
    }
    return 0;
}

static int cmd_wifi_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    bool initialized = wifi_driver_is_initialized();
    char saved_ssid[33] = {0};
    char saved_pass[65] = {0};
    bool has_saved = wifi_driver_get_saved_credentials(saved_ssid, sizeof(saved_ssid),
                                                       saved_pass, sizeof(saved_pass));

    wifi_driver_status_t st = WIFI_STATUS_IDLE;
    wifi_ap_record_t ap = {0};
    esp_err_t err = wifi_driver_get_status(&st, &ap);
    const char *st_str = (st == WIFI_STATUS_IDLE) ? "idle" :
                         (st == WIFI_STATUS_CONNECTING) ? "connecting" :
                         (st == WIFI_STATUS_CONNECTED) ? "connected" : "failed";
    printf("Driver: initialized=%s saved_credentials=%s\n",
           initialized ? "yes" : "no",
           has_saved ? "yes" : "no");
    printf("Status: %s (err=%s)\n", st_str, esp_err_to_name(err));
    if (!initialized) {
        printf("Hint: run `wifi_init` (or enable auto WiFi with saved SSID/pass).\n");
    } else if (st == WIFI_STATUS_IDLE && has_saved) {
        printf("Hint: credentials exist but no connect attempt is active. Run `wifi_connect <ssid> <pass>`.\n");
    }
    if (st == WIFI_STATUS_CONNECTED) {
        printf("Connected to SSID=\"%s\" RSSI=%d CH=%d BSSID=%02X:%02X:%02X:%02X:%02X:%02X\n",
               ap.ssid, ap.rssi, ap.primary,
               ap.bssid[0], ap.bssid[1], ap.bssid[2],
               ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }
    return 0;
}

static int cmd_wifi_ping(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: wifi_ping <host> [count] [timeout_ms]\n");
        return 0;
    }
    uint32_t count = 4;
    uint32_t timeout_ms = 1000;
    if (argc >= 3) {
        count = (uint32_t)atoi(argv[2]);
        if (count == 0) count = 4;
    }
    if (argc >= 4) {
        timeout_ms = (uint32_t)atoi(argv[3]);
        if (timeout_ms == 0) timeout_ms = 1000;
    }
    wifi_ping_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        printf("Ping alloc failed\n");
        return 0;
    }
    strlcpy(args->host, argv[1], sizeof(args->host));
    args->count = count;
    args->timeout_ms = timeout_ms;

    BaseType_t ok = xTaskCreatePinnedToCore(wifi_ping_task, "wifi_ping", 4096, args, 3, NULL, 0);
    if (ok != pdPASS) {
        free(args);
        printf("Ping task create failed\n");
        return 0;
    }
    printf("Ping started for %s\n", args->host);
    return 0;
}

esp_err_t ConsoleWiFi_Init(void)
{
    const esp_console_cmd_t init_cmd = {
        .command = "wifi_init",
        .help = "Initialize WiFi STA",
        .hint = NULL,
        .func = &cmd_wifi_init,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&init_cmd), TAG, "register wifi_init failed");

    const esp_console_cmd_t scan_cmd = {
        .command = "wifi_scan",
        .help = "Scan for WiFi APs (logs SSID/RSSI/CH)",
        .hint = NULL,
        .func = &cmd_wifi_scan,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&scan_cmd), TAG, "register wifi_scan failed");

    const esp_console_cmd_t connect_cmd = {
        .command = "wifi_connect",
        .help = "Connect to strongest BSSID for SSID. Usage: wifi_connect <ssid> <pass>",
        .hint = NULL,
        .func = &cmd_wifi_connect,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&connect_cmd), TAG, "register wifi_connect failed");

    const esp_console_cmd_t roam_cmd = {
        .command = "wifi_roam",
        .help = "Toggle roaming to strongest BSSID. Usage: wifi_roam <on|off>",
        .hint = NULL,
        .func = &cmd_wifi_roam,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&roam_cmd), TAG, "register wifi_roam failed");

    const esp_console_cmd_t status_cmd = {
        .command = "wifi_status",
        .help = "Show WiFi status",
        .hint = NULL,
        .func = &cmd_wifi_status,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&status_cmd), TAG, "register wifi_status failed");

    const esp_console_cmd_t ping_cmd = {
        .command = "wifi_ping",
        .help = "Ping a host. Usage: wifi_ping <host> [count] [timeout_ms]",
        .hint = NULL,
        .func = &cmd_wifi_ping,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ping_cmd), TAG, "register wifi_ping failed");

    EL_LOGI(TAG, "WiFi console commands ready");
    return ESP_OK;
}

