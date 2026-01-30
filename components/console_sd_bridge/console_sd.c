#include "console_sd.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_console.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_driver.h"
#include "config_store.h"
#include "eldra_cloud.h"
#include "esp_http_client.h"
#include "wifi_driver.h"

static const char *TAG = "console_sd";

static char *join_args(int argc, char **argv, int start)
{
    if (argc <= start) return NULL;
    size_t len = 0;
    for (int i = start; i < argc; i++) {
        len += strlen(argv[i]) + 1;
    }
    char *out = malloc(len);
    if (!out) return NULL;
    out[0] = '\0';
    for (int i = start; i < argc; i++) {
        strcat(out, argv[i]);
        if (i != argc - 1) strcat(out, " ");
    }
    return out;
}

static int cmd_sd_init(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t ret = sd_driver_init();
    if (ret != ESP_OK) {
        printf("SD init failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("SD init OK\n");
    }
    return 0;
}

static int cmd_sd_list(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : NULL;
    esp_err_t ret = sd_driver_list_async(path);
    if (ret != ESP_OK) {
        printf("SD list enqueue failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("Listing queued\n");
    }
    return 0;
}

static int cmd_sd_read(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: sd_read <path>\n");
        return 0;
    }
    esp_err_t ret = sd_driver_read_async(argv[1]);
    if (ret != ESP_OK) {
        printf("Read enqueue failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("Read queued for %s\n", argv[1]);
    }
    return 0;
}

static int cmd_sd_delete(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: sd_delete <path>\n");
        return 0;
    }
    esp_err_t ret = sd_driver_delete_async(argv[1]);
    if (ret != ESP_OK) {
        printf("Delete enqueue failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("Delete queued for %s\n", argv[1]);
    }
    return 0;
}

static int cmd_sd_log(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: sd_log <text>\n");
        return 0;
    }
    char *joined = join_args(argc, argv, 1);
    if (!joined) {
        printf("Allocation failed\n");
        return 0;
    }
    esp_err_t ret = sd_driver_log_async(joined);
    free(joined);
    if (ret != ESP_OK) {
        printf("Log enqueue failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("Log queued\n");
    }
    return 0;
}

static int cmd_sd_wifi_save(int argc, char **argv)
{
    const char *ssid = NULL;
    const char *pass = NULL;
    char buf_ssid[33];
    char buf_pass[65];
    if (argc >= 3) {
        ssid = argv[1];
        pass = argv[2];
    } else if (wifi_driver_get_saved_credentials(buf_ssid, sizeof(buf_ssid), buf_pass, sizeof(buf_pass))) {
        ssid = buf_ssid;
        pass = buf_pass;
        printf("Using current connection credentials\n");
    } else {
        printf("Usage: sd_wifi_save <ssid> <pass> (or run while connected)\n");
        return 0;
    }
    esp_err_t ret = sd_driver_save_wifi_credentials(ssid, pass);
    if (ret != ESP_OK) {
        printf("WiFi save failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("WiFi credentials queued to save\n");
    }

    // Also persist into CONFIG so auto-init can use it.
    config_store_t cfg = {0};
    if (config_store_load(&cfg) == ESP_OK) {
        strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
        strlcpy(cfg.wifi_pass, pass, sizeof(cfg.wifi_pass));
        esp_err_t cr = config_store_save(&cfg);
        if (cr != ESP_OK) {
            printf("Config update failed: %s\n", esp_err_to_name(cr));
        } else {
            printf("Config updated with WiFi credentials\n");
        }
    } else {
        printf("Config load failed; not updating config file\n");
    }
    return 0;
}

static int cmd_config_show(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (sd_driver_init() != ESP_OK) {
        printf("SD not ready\n");
        return 0;
    }
    config_store_t cfg;
    esp_err_t ret = config_store_load(&cfg);
    if (ret != ESP_OK) {
        printf("Load config failed: %s\n", esp_err_to_name(ret));
        return 0;
    }
    printf("Config:\n");
    printf("  auto_init: sd=%s wifi=%s rtc=%s imu=%s\n",
           cfg.auto_init_sd ? "true" : "false",
           cfg.auto_init_wifi ? "true" : "false",
           cfg.auto_init_rtc ? "true" : "false",
           cfg.auto_init_imu ? "true" : "false");
    printf("  wifi: ssid=\"%s\" pass=%s roam=%s\n",
           cfg.wifi_ssid,
           (cfg.wifi_pass[0] ? "***" : "(empty)"),
           cfg.wifi_roam ? "true" : "false");
    printf("  logs: to_sd=%s cloud_console=%s\n",
           cfg.logs_to_sd ? "true" : "false",
           cfg.cloud_logs_console ? "true" : "false");
    printf("  cloud: auto=%s url=\"%s\" token=%s\n",
           cfg.auto_init_cloud ? "true" : "false",
           cfg.cloud_base_url,
           cfg.cloud_token[0] ? "***" : "(empty)");
    printf("  cloud_intervals_ms: poll=%d state=%d log=%d\n",
           cfg.cloud_poll_interval_ms, cfg.cloud_state_interval_ms, cfg.cloud_log_interval_ms);
    printf("  eyes: center_x_offset=%d center_y_offset=%d display_center_x_offset=%d display_center_y_offset=%d\n",
           cfg.eyes_center_x_offset, cfg.eyes_center_y_offset,
           cfg.display_center_x_offset, cfg.display_center_y_offset);
    printf("  sleep: start_hour=%d end_hour=%d\n", cfg.sleep_start_hour, cfg.sleep_end_hour);
    printf("  emotion: mood_log_interval_minutes=%d\n", cfg.mood_log_interval_minutes);
    return 0;
}

static int cmd_sd_format(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        printf("WARNING: This will erase the SD card. Usage: sd_format yes\n");
        return 0;
    }
    esp_err_t ret = sd_driver_format();
    if (ret != ESP_OK) {
        printf("Format failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("Format complete\n");
    }
    return 0;
}

static int cmd_cloud_set(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cloud_set <base_url> [token]\n");
        printf("Or:    cloud_set token <token>\n");
        return 0;
    }
    config_store_t cfg;
    if (config_store_load(&cfg) != ESP_OK) {
        printf("Load config failed\n");
        return 0;
    }

    if (strcasecmp(argv[1], "token") == 0) {
        if (argc < 3) {
            printf("Usage: cloud_set token <token>\n");
            return 0;
        }
        strlcpy(cfg.cloud_token, argv[2], sizeof(cfg.cloud_token));
        cfg.auto_init_cloud = true;
        if (config_store_save(&cfg) != ESP_OK) {
            printf("Save config failed\n");
        } else {
            printf("Cloud token saved (auto_init_cloud=true)\n");
        }
        return 0;
    }

    // Set base URL and optionally token.
    strlcpy(cfg.cloud_base_url, argv[1], sizeof(cfg.cloud_base_url));
    if (argc >= 3) {
        strlcpy(cfg.cloud_token, argv[2], sizeof(cfg.cloud_token));
    }
    cfg.auto_init_cloud = true;
    if (config_store_save(&cfg) != ESP_OK) {
        printf("Save config failed\n");
    } else {
        printf("Cloud config saved (auto_init_cloud=true)%s\n",
               (argc >= 3) ? " with new token" : " (token unchanged)");
    }
    return 0;
}

static void cloud_health_task(void *arg)
{
    (void)arg;
    bool ok = eldra_cloud_health_check();
    printf("Cloud health: %s\n", ok ? "OK" : "FAILED");
    vTaskDelete(NULL);
}

static int cmd_cloud_health(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Cloud health: starting (base=%s token=%s)\n", "(see config)", "(hidden)");
    if (xTaskCreate(cloud_health_task, "cloud_health", 4096, NULL, 4, NULL) != pdPASS) {
        printf("Cloud health: task create failed\n");
    }
    return 0;
}

static int cmd_ping_url(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: ping <url>\n");
        return 0;
    }
    const char *url = argv[1];
    printf("Ping HTTP GET %s ...\n", url);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 3000,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        printf("Ping init failed\n");
        return 0;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && status > 0) {
        printf("Ping %s -> HTTP %d\n", url, status);
    } else {
        printf("Ping %s failed: %s (status=%d)\n", url, esp_err_to_name(err), status);
    }
    return 0;
}

static int cmd_config_set_eyes(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: config_set_eyes <x_offset> <y_offset>\n");
        return 0;
    }
    int x = (int)strtol(argv[1], NULL, 10);
    int y = (int)strtol(argv[2], NULL, 10);
    config_store_t cfg;
    if (config_store_load(&cfg) != ESP_OK) {
        printf("Load config failed\n");
        return 0;
    }
    cfg.eyes_center_x_offset = x;
    cfg.eyes_center_y_offset = y;
    if (config_store_save(&cfg) != ESP_OK) {
        printf("Save config failed\n");
    } else {
        printf("Saved eyes offsets to config (x=%d y=%d)\n", x, y);
    }
    return 0;
}

static int cmd_cloud_logs_console(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cloud_logs_console <on|off>\n");
        return 0;
    }
    bool enable = (strcasecmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0 || strcasecmp(argv[1], "true") == 0);
    config_store_t cfg;
    if (config_store_load(&cfg) != ESP_OK) {
        printf("Load config failed\n");
        return 0;
    }
    cfg.cloud_logs_console = enable;
    if (config_store_save(&cfg) != ESP_OK) {
        printf("Save config failed\n");
        return 0;
    }
    printf("Cloud console logging %s and saved\n", enable ? "enabled" : "disabled");
    // Apply immediately if cloud is initialized
    eldra_cloud_set_console_logging(enable);
    return 0;
}

static int cmd_cloud_on(int argc, char **argv)
{
    (void)argc; (void)argv;
    eldra_cloud_set_online(true);
    printf("Cloud worker requested online\n");
    return 0;
}

static int cmd_cloud_off(int argc, char **argv)
{
    (void)argc; (void)argv;
    eldra_cloud_set_online(false);
    printf("Cloud worker set offline\n");
    return 0;
}

static int cmd_cloud_periods(int argc, char **argv)
{
    if (argc < 4) {
        printf("Usage: cloud_periods <poll_ms> <state_ms> <log_ms>\n");
        return 0;
    }
    int poll = (int)strtol(argv[1], NULL, 10);
    int state = (int)strtol(argv[2], NULL, 10);
    int logi = (int)strtol(argv[3], NULL, 10);

    config_store_t cfg;
    if (config_store_load(&cfg) != ESP_OK) {
        printf("Load config failed\n");
        return 0;
    }
    cfg.cloud_poll_interval_ms = poll;
    cfg.cloud_state_interval_ms = state;
    cfg.cloud_log_interval_ms = logi;
    if (config_store_save(&cfg) != ESP_OK) {
        printf("Save config failed\n");
    } else {
        printf("Cloud intervals saved (poll=%d state=%d log=%d)\n", poll, state, logi);
    }
    eldra_cloud_set_intervals(poll, state, logi);
    return 0;
}

static int cmd_cloud_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    eldra_cloud_status_t st = {0};
    eldra_cloud_get_status(&st);
    printf("Cloud status:\n");
    printf("  online_requested=%d online=%d failures=%d backoff_ms=%lld\n",
           st.online_requested, st.online, st.health_failures, (long long)st.health_backoff_ms);
    printf("  last_health: ok=%d t=%lldms\n", st.last_health_ok, (long long)st.last_health_time_ms);
    printf("  last_state: ok=%d t=%lldms\n", st.last_state_ok, (long long)st.last_state_time_ms);
    printf("  last_cmd:   ok=%d t=%lldms\n", st.last_cmd_ok, (long long)st.last_cmd_time_ms);
    printf("  intervals_ms: poll=%d state=%d log=%d\n",
           st.poll_interval_ms, st.state_interval_ms, st.log_interval_ms);
    return 0;
}

esp_err_t ConsoleSD_Init(void)
{
    const esp_console_cmd_t init_cmd = {
        .command = "sd_init",
        .help = "Mount SD card",
        .hint = NULL,
        .func = &cmd_sd_init,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&init_cmd), TAG, "register sd_init failed");

    const esp_console_cmd_t list_cmd = {
        .command = "sd_list",
        .help = "List files on SD. Usage: sd_list [path]",
        .hint = NULL,
        .func = &cmd_sd_list,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&list_cmd), TAG, "register sd_list failed");

    const esp_console_cmd_t read_cmd = {
        .command = "sd_read",
        .help = "Read file and print to console. Usage: sd_read <path>",
        .hint = NULL,
        .func = &cmd_sd_read,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&read_cmd), TAG, "register sd_read failed");

    const esp_console_cmd_t del_cmd = {
        .command = "sd_delete",
        .help = "Delete file. Usage: sd_delete <path>",
        .hint = NULL,
        .func = &cmd_sd_delete,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&del_cmd), TAG, "register sd_delete failed");

    const esp_console_cmd_t log_cmd = {
        .command = "sd_log",
        .help = "Append a line to daily log. Usage: sd_log <text>",
        .hint = NULL,
        .func = &cmd_sd_log,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&log_cmd), TAG, "register sd_log failed");

    const esp_console_cmd_t wifi_cmd = {
        .command = "sd_wifi_save",
        .help = "Save WiFi credentials to SD. Usage: sd_wifi_save <ssid> <pass>",
        .hint = NULL,
        .func = &cmd_sd_wifi_save,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&wifi_cmd), TAG, "register sd_wifi_save failed");

    const esp_console_cmd_t cfg_show_cmd = {
        .command = "config_show",
        .help = "Show /sdcard/config.json (parsed)",
        .hint = NULL,
        .func = &cmd_config_show,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cfg_show_cmd), TAG, "register config_show failed");

    const esp_console_cmd_t fmt_cmd = {
        .command = "sd_format",
        .help = "Format SD card (destructive). Usage: sd_format yes",
        .hint = NULL,
        .func = &cmd_sd_format,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&fmt_cmd), TAG, "register sd_format failed");

    const esp_console_cmd_t cloud_set_cmd = {
        .command = "cloud_set",
        .help = "Save cloud base URL and token to config. Usage: cloud_set <url> <token>",
        .hint = NULL,
        .func = &cmd_cloud_set,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cloud_set_cmd), TAG, "register cloud_set failed");

    const esp_console_cmd_t cloud_health_cmd = {
        .command = "cloud_health",
        .help = "GET /api/health with current cloud config.",
        .hint = NULL,
        .func = &cmd_cloud_health,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cloud_health_cmd), TAG, "register cloud_health failed");

    const esp_console_cmd_t ping_cmd = {
        .command = "ping",
        .help = "HTTP GET a URL to test reachability. Usage: ping <url>",
        .hint = NULL,
        .func = &cmd_ping_url,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ping_cmd), TAG, "register ping failed");

    const esp_console_cmd_t eyes_set_cmd = {
        .command = "config_set_eyes",
        .help = "Persist eye center offsets. Usage: config_set_eyes <x> <y>",
        .hint = NULL,
        .func = &cmd_config_set_eyes,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&eyes_set_cmd), TAG, "register config_set_eyes failed");

    const esp_console_cmd_t cloud_logs_cmd = {
        .command = "cloud_logs_console",
        .help = "Toggle eldra_cloud logs to console. Usage: cloud_logs_console <on|off>",
        .hint = NULL,
        .func = &cmd_cloud_logs_console,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cloud_logs_cmd), TAG, "register cloud_logs_console failed");

    const esp_console_cmd_t cloud_on_cmd = {
        .command = "cloud_on",
        .help = "Mark cloud worker online (will run health/poll/state/log loops)",
        .hint = NULL,
        .func = &cmd_cloud_on,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cloud_on_cmd), TAG, "register cloud_on failed");

    const esp_console_cmd_t cloud_off_cmd = {
        .command = "cloud_off",
        .help = "Mark cloud worker offline (pauses health/poll/state/log loops)",
        .hint = NULL,
        .func = &cmd_cloud_off,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cloud_off_cmd), TAG, "register cloud_off failed");

    const esp_console_cmd_t cloud_periods_cmd = {
        .command = "cloud_periods",
        .help = "Set cloud intervals (ms). Usage: cloud_periods <poll_ms> <state_ms> <log_ms>",
        .hint = NULL,
        .func = &cmd_cloud_periods,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cloud_periods_cmd), TAG, "register cloud_periods failed");

    const esp_console_cmd_t cloud_status_cmd = {
        .command = "cloud_status",
        .help = "Show cloud worker status/intervals/last success times",
        .hint = NULL,
        .func = &cmd_cloud_status,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cloud_status_cmd), TAG, "register cloud_status failed");

    ESP_LOGI(TAG, "SD console commands ready");
    return ESP_OK;
}
