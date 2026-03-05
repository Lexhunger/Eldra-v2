#include "console_sd.h"

#include <string.h>
#include <strings.h>
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
#include "eldra_emotion.h"
#include "eldra_sleep.h"
#include "eldra_eyes.h"
#include "eldra_glyphs.h"
#include "eldra_display_round.h"
#include "eldra_logging.h"
#include "eldra_comms.h"
#include "runtime_monitor.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "wifi_driver.h"

static const char *TAG = "console_sd";
typedef struct {
    char url[256];
} ping_http_args_t;

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
    printf("  eyes: center_x_offset=%d center_y_offset=%d display_center_x_offset=%d display_center_y_offset=%d glyph_offset_x=%d glyph_offset_y=%d glyph_scale=%d sleep_lid_depth=%d angry_lid_depth=%d\n",
           cfg.eyes_center_x_offset, cfg.eyes_center_y_offset,
           cfg.display_center_x_offset, cfg.display_center_y_offset,
           cfg.glyph_offset_x, cfg.glyph_offset_y, cfg.glyph_scale,
           cfg.sleep_lid_depth, cfg.angry_lid_depth);
    printf("  sleep: start_hour=%d end_hour=%d window_idle_ms=%d low_battery_pct=%d global_idle_ms=%d overfed_hold_ms=%d\n",
           cfg.sleep_start_hour, cfg.sleep_end_hour,
           cfg.sleep_window_inactivity_ms, cfg.sleep_low_battery_pct,
           cfg.sleep_global_inactivity_ms, cfg.sleep_overfed_hold_ms);
    printf("  emotion: mood_log_interval_minutes=%d angry_dizzy_count_threshold=%d angry_dizzy_window_ms=%d angry_override_min_ms=%d angry_override_max_ms=%d\n",
           cfg.mood_log_interval_minutes,
           cfg.angry_dizzy_count_threshold,
           cfg.angry_dizzy_window_ms,
           cfg.angry_override_min_ms,
           cfg.angry_override_max_ms);
    return 0;
}

static bool parse_log_level_cli(const char *s, log_level_t *out)
{
    if (!s || !out) {
        return false;
    }
    if (strcasecmp(s, "DEBUG") == 0) {
        *out = LOG_LEVEL_DEBUG;
        return true;
    }
    if (strcasecmp(s, "INFO") == 0) {
        *out = LOG_LEVEL_INFO;
        return true;
    }
    if (strcasecmp(s, "WARN") == 0) {
        *out = LOG_LEVEL_WARN;
        return true;
    }
    if (strcasecmp(s, "ERROR") == 0) {
        *out = LOG_LEVEL_ERROR;
        return true;
    }
    return false;
}

static const char *log_level_to_str(log_level_t lvl)
{
    switch (lvl) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default:              return "?";
    }
}

typedef struct {
    const char *name;
    const char *const *tags;
    size_t tag_count;
} log_focus_group_t;

static const char *const k_focus_scaffold[] = {
    "app_main", "cmd_router", "comms", "console", "jobs",
    "console_sd", "console_wifi", "console_rtc", "console_imu", "console_emotion"
};

static const char *const k_focus_display[] = {
    "eldra_display_round", "LCD", "eldra_eyes", "eldra_glyphs", "backlight", "exio"
};

static const char *const k_focus_sleep[] = {
    "eldra_sleep", "emotion", "eldra_eyes"
};

static const char *const k_focus_cloud[] = {
    "eldra_cloud", "cmd_router", "console_sd"
};

static const char *const k_focus_wifi[] = {
    "wifi_drv", "console_wifi", "eldra_cloud", "app_main"
};

static const char *const k_focus_sensors[] = {
    "eldra_sensors", "qmi8658", "ADC", "I2C", "console_imu"
};

static const char *const k_focus_storage[] = {
    "sd_drv", "config_store", "eldra_logging", "rtc_drv", "console_sd"
};

static const log_focus_group_t k_log_focus_groups[] = {
    {.name = "scaffold", .tags = k_focus_scaffold, .tag_count = sizeof(k_focus_scaffold) / sizeof(k_focus_scaffold[0])},
    {.name = "display",  .tags = k_focus_display,  .tag_count = sizeof(k_focus_display) / sizeof(k_focus_display[0])},
    {.name = "sleep",    .tags = k_focus_sleep,    .tag_count = sizeof(k_focus_sleep) / sizeof(k_focus_sleep[0])},
    {.name = "cloud",    .tags = k_focus_cloud,    .tag_count = sizeof(k_focus_cloud) / sizeof(k_focus_cloud[0])},
    {.name = "wifi",     .tags = k_focus_wifi,     .tag_count = sizeof(k_focus_wifi) / sizeof(k_focus_wifi[0])},
    {.name = "sensors",  .tags = k_focus_sensors,  .tag_count = sizeof(k_focus_sensors) / sizeof(k_focus_sensors[0])},
    {.name = "storage",  .tags = k_focus_storage,  .tag_count = sizeof(k_focus_storage) / sizeof(k_focus_storage[0])},
};

static const log_focus_group_t *find_log_focus_group(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_log_focus_groups) / sizeof(k_log_focus_groups[0]); ++i) {
        if (strcasecmp(name, k_log_focus_groups[i].name) == 0) {
            return &k_log_focus_groups[i];
        }
    }
    return NULL;
}

static int cmd_log_level(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: loglevel <DEBUG|INFO|WARN|ERROR>\n");
        return 0;
    }
    log_level_t lvl = LOG_LEVEL_INFO;
    if (!parse_log_level_cli(argv[1], &lvl)) {
        printf("Invalid level. Use DEBUG|INFO|WARN|ERROR\n");
        return 0;
    }
    log_set_console_level(lvl);
    printf("Console log level set\n");
    return 0;
}

static int cmd_log_tag(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: logtag <tag> <on|off> [DEBUG|INFO|WARN|ERROR]\n");
        return 0;
    }

    bool enable = false;
    if (strcasecmp(argv[2], "on") == 0) {
        enable = true;
    } else if (strcasecmp(argv[2], "off") == 0) {
        enable = false;
    } else {
        printf("Invalid mode. Use on|off\n");
        return 0;
    }

    log_level_t lvl = LOG_LEVEL_INFO;
    if (argc >= 4 && !parse_log_level_cli(argv[3], &lvl)) {
        printf("Invalid level. Use DEBUG|INFO|WARN|ERROR\n");
        return 0;
    }

    if (!log_console_set_tag_rule(argv[1], enable, lvl)) {
        printf("Failed to set tag rule (table full?)\n");
        return 0;
    }
    printf("logtag set: %s %s level>=%s\n", argv[1], enable ? "on" : "off",
           (lvl == LOG_LEVEL_DEBUG) ? "DEBUG" :
           (lvl == LOG_LEVEL_INFO) ? "INFO" :
           (lvl == LOG_LEVEL_WARN) ? "WARN" : "ERROR");
    return 0;
}

static int cmd_log_tag_clear(int argc, char **argv)
{
    if (argc >= 2) {
        log_console_clear_tag_rule(argv[1]);
        printf("logtag rule cleared: %s\n", argv[1]);
    } else {
        log_console_clear_all_tag_rules();
        printf("all logtag rules cleared\n");
    }
    return 0;
}

static int cmd_log_tags(int argc, char **argv)
{
    size_t limit = 64;
    if (argc >= 2) {
        long parsed = strtol(argv[1], NULL, 10);
        if (parsed > 0) {
            limit = (size_t)parsed;
        }
    }
    if (limit > 64) {
        limit = 64;
    }

    log_tag_info_t info[64];
    size_t total = 0;
    size_t shown = log_get_tag_snapshot(info, limit, &total);
    printf("Known log tags: total=%u showing=%u\n", (unsigned)total, (unsigned)shown);
    if (shown == 0) {
        printf("  (no tags observed yet)\n");
        return 0;
    }

    for (size_t i = 0; i < shown; ++i) {
        const char *rule = info[i].has_console_rule ? "rule" : "default";
        printf("  %-16s seen=%-6u last=%-10ums console=%s >=%s (%s)\n",
               info[i].tag,
               (unsigned)info[i].seen_count,
               (unsigned)info[i].last_seen_ms,
               info[i].console_enabled ? "on" : "off",
               log_level_to_str(info[i].console_min_level),
               rule);
    }
    if (shown < total) {
        printf("  ... truncated (use `logtags %u`)\n", (unsigned)total);
    }
    return 0;
}

static int cmd_log_focus(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: logfocus <off|scaffold|display|sleep|cloud|wifi|sensors|storage>\n");
        return 0;
    }

    if (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "all") == 0) {
        log_console_clear_all_tag_rules();
        log_set_console_level(LOG_LEVEL_INFO);
        printf("logfocus disabled: console level=INFO, tag rules cleared\n");
        return 0;
    }

    const log_focus_group_t *group = find_log_focus_group(argv[1]);
    if (!group) {
        printf("Unknown group '%s'. Use one of: off scaffold display sleep cloud wifi sensors storage\n", argv[1]);
        return 0;
    }

    log_console_clear_all_tag_rules();
    // Keep non-focused subsystems quiet but still show WARN/ERROR globally.
    log_set_console_level(LOG_LEVEL_WARN);

    bool ok = true;
    // Always keep console bridge chatter visible while focused.
    ok &= log_console_set_tag_rule("console_sd", true, LOG_LEVEL_INFO);
    ok &= log_console_set_tag_rule("cmd_router", true, LOG_LEVEL_INFO);
    for (size_t i = 0; i < group->tag_count; ++i) {
        ok &= log_console_set_tag_rule(group->tags[i], true, LOG_LEVEL_DEBUG);
    }

    printf("logfocus=%s (global>=WARN, %u focused tags >=DEBUG)%s\n",
           group->name, (unsigned)group->tag_count, ok ? "" : " [rule table full]");
    return 0;
}

static int cmd_bridge_stats(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    comms_stats_t st = {0};
    comms_get_stats(&st);
    printf("Bridge stats:\n");
    printf("  queue_depth=%u max_depth=%u capacity=%u\n",
           (unsigned)st.queue_depth,
           (unsigned)st.queue_max_depth,
           16U);
    printf("  enqueue_ok=%u drop_full=%u drop_lock=%u dequeue_ok=%u\n",
           (unsigned)st.enqueue_ok,
           (unsigned)st.enqueue_drop_full,
           (unsigned)st.enqueue_drop_lock,
           (unsigned)st.dequeue_ok);
    printf("  dequeue_latency_ms last=%u avg=%u max=%u\n",
           (unsigned)st.dequeue_latency_last_ms,
           (unsigned)st.dequeue_latency_avg_ms,
           (unsigned)st.dequeue_latency_max_ms);
    return 0;
}

static int cmd_monitor_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    runtime_monitor_config_t cfg = {0};
    runtime_monitor_stats_t st = {0};
    runtime_monitor_get_config(&cfg);
    runtime_monitor_get_stats(&st);

    printf("Monitor config:\n");
    printf("  snapshot_interval_ms=%u sd_snapshot=%s console_snapshot=%s\n",
           (unsigned)cfg.snapshot_interval_ms,
           cfg.sd_snapshot_enabled ? "true" : "false",
           cfg.console_snapshot_enabled ? "true" : "false");
    printf("  stall_threshold_ms=%u stall_hits_before_restart=%u auto_restart=%s\n",
           (unsigned)cfg.stall_threshold_ms,
           (unsigned)cfg.stall_hits_before_restart,
           cfg.auto_restart_on_stall ? "true" : "false");

    printf("Monitor stats:\n");
    printf("  snapshots=%u last_snapshot_ms=%u stall_hits(main=%u display=%u) restarts=%u\n",
           (unsigned)st.snapshot_count,
           (unsigned)st.last_snapshot_ms,
           (unsigned)st.main_stall_hits,
           (unsigned)st.display_stall_hits,
           (unsigned)st.stall_restarts);
    printf("  heap(int=%u min=%u ps=%u min=%u)\n",
           (unsigned)st.free_heap_internal,
           (unsigned)st.min_heap_internal,
           (unsigned)st.free_heap_spiram,
           (unsigned)st.min_heap_spiram);
    printf("  stack_hwm_words(main=%u display=%u monitor=%u)\n",
           (unsigned)st.main_stack_hwm_words,
           (unsigned)st.display_stack_hwm_words,
           (unsigned)st.monitor_stack_hwm_words);
    printf("  bridge(depth=%u max=%u drop_full=%u drop_lock=%u)\n",
           (unsigned)st.bridge_queue_depth,
           (unsigned)st.bridge_queue_max_depth,
           (unsigned)st.bridge_drop_full,
           (unsigned)st.bridge_drop_lock);
    printf("  bridge_latency_ms(last=%u avg=%u max=%u)\n",
           (unsigned)st.bridge_dequeue_latency_last_ms,
           (unsigned)st.bridge_dequeue_latency_avg_ms,
           (unsigned)st.bridge_dequeue_latency_max_ms);
    printf("  panel(blit_fail=%u blit_timeout=%u sync=%u reset=%u)\n",
           (unsigned)st.panel_blit_fail_count,
           (unsigned)st.panel_blit_timeout_count,
           (unsigned)st.panel_sync_count,
           (unsigned)st.panel_reset_count);
    return 0;
}

static int cmd_monitor_cfg(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: monitor_cfg <snapshot_ms> [stall_ms] [hits] [auto_restart 0|1] [console_snap 0|1] [sd_snap 0|1]\n");
        return 0;
    }

    runtime_monitor_config_t cfg = {0};
    runtime_monitor_get_config(&cfg);

    cfg.snapshot_interval_ms = (uint32_t)strtoul(argv[1], NULL, 10);
    if (argc >= 3) cfg.stall_threshold_ms = (uint32_t)strtoul(argv[2], NULL, 10);
    if (argc >= 4) cfg.stall_hits_before_restart = (uint8_t)strtoul(argv[3], NULL, 10);
    if (argc >= 5) cfg.auto_restart_on_stall = (strtoul(argv[4], NULL, 10) != 0);
    if (argc >= 6) cfg.console_snapshot_enabled = (strtoul(argv[5], NULL, 10) != 0);
    if (argc >= 7) cfg.sd_snapshot_enabled = (strtoul(argv[6], NULL, 10) != 0);

    runtime_monitor_set_config(&cfg);
    printf("Monitor config updated: snapshot=%u stall=%u hits=%u auto_restart=%s console=%s sd=%s\n",
           (unsigned)cfg.snapshot_interval_ms,
           (unsigned)cfg.stall_threshold_ms,
           (unsigned)cfg.stall_hits_before_restart,
           cfg.auto_restart_on_stall ? "true" : "false",
           cfg.console_snapshot_enabled ? "true" : "false",
           cfg.sd_snapshot_enabled ? "true" : "false");
    return 0;
}

static int cmd_config_clear_all(int argc, char **argv)
{
    bool do_reboot = false;
    bool preserve_wifi = false;
    for (int i = 1; i < argc; ++i) {
        if ((strcasecmp(argv[i], "reboot") == 0) ||
            (strcmp(argv[i], "1") == 0) ||
            (strcasecmp(argv[i], "true") == 0)) {
            do_reboot = true;
        } else if (strcasecmp(argv[i], "preserve_wifi") == 0) {
            preserve_wifi = true;
        }
    }

    config_store_t old_cfg = {0};
    bool old_cfg_ok = (config_store_load(&old_cfg) == ESP_OK);
    if (preserve_wifi && (!old_cfg_ok || old_cfg.wifi_ssid[0] == '\0')) {
        printf("preserve_wifi requested but no saved WiFi credentials found.\n");
    }

    esp_err_t ret = config_store_factory_reset();
    if (ret != ESP_OK) {
        printf("config_clear_all failed: %s\n", esp_err_to_name(ret));
        return 0;
    }

    // Apply defaults immediately for current runtime session.
    config_store_t cfg = {0};
    config_store_get_defaults(&cfg);
    if (preserve_wifi && old_cfg_ok && old_cfg.wifi_ssid[0] != '\0') {
        strlcpy(cfg.wifi_ssid, old_cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
        strlcpy(cfg.wifi_pass, old_cfg.wifi_pass, sizeof(cfg.wifi_pass));
        cfg.wifi_roam = old_cfg.wifi_roam;
        cfg.auto_init_wifi = old_cfg.auto_init_wifi;
        (void)config_store_save(&cfg);
    }
    eldra_glyphs_set_layout(GLYPH_LAYOUT_STACK);
    eldra_glyphs_hide_all();
    eldra_eyes_set_display_center_offset(cfg.display_center_x_offset, cfg.display_center_y_offset);
    eldra_glyphs_set_display_center_offset(cfg.display_center_x_offset, cfg.display_center_y_offset);
    eldra_eyes_set_center_offset(cfg.eyes_center_x_offset, cfg.eyes_center_y_offset);
    eldra_eyes_set_lid_depths((uint8_t)cfg.sleep_lid_depth, (uint8_t)cfg.angry_lid_depth);
    eldra_glyphs_set_offset(0, cfg.glyph_offset_x, cfg.glyph_offset_y);
    eldra_glyphs_set_scale(cfg.glyph_scale);
    emotion_set_sleep_window((uint8_t)cfg.sleep_start_hour, (uint8_t)cfg.sleep_end_hour);
    eldra_sleep_set_window((uint8_t)cfg.sleep_start_hour, (uint8_t)cfg.sleep_end_hour);
    eldra_sleep_set_thresholds((uint32_t)cfg.sleep_window_inactivity_ms,
                               (uint8_t)cfg.sleep_low_battery_pct,
                               (uint32_t)cfg.sleep_global_inactivity_ms,
                               (uint32_t)cfg.sleep_overfed_hold_ms);
    emotion_set_mood_log_interval_minutes((uint32_t)cfg.mood_log_interval_minutes);
    emotion_set_angry_policy((uint8_t)cfg.angry_dizzy_count_threshold,
                             (uint32_t)cfg.angry_dizzy_window_ms,
                             (uint32_t)cfg.angry_override_min_ms,
                             (uint32_t)cfg.angry_override_max_ms);

    int eff_x = cfg.eyes_center_x_offset;
    int eff_y = cfg.eyes_center_y_offset;
    eldra_eyes_get_effective_center_offset(&eff_x, &eff_y);
    printf("All presets cleared. Defaults restored.\n");
    printf("Applied defaults: eyes=(%d,%d) glyph=(%d,%d) scale=%d lids=(sleep=%d angry=%d) sleep=%d-%d mood_log=%dmin angry_policy=(count>%d window=%dms hold=%d..%dms)\n",
           eff_x, eff_y, cfg.glyph_offset_x, cfg.glyph_offset_y, cfg.glyph_scale,
           cfg.sleep_lid_depth, cfg.angry_lid_depth,
           cfg.sleep_start_hour, cfg.sleep_end_hour, cfg.mood_log_interval_minutes,
           cfg.angry_dizzy_count_threshold, cfg.angry_dizzy_window_ms,
           cfg.angry_override_min_ms, cfg.angry_override_max_ms);
    if (preserve_wifi && old_cfg_ok && old_cfg.wifi_ssid[0] != '\0') {
        printf("WiFi preserved: ssid=\"%s\" roam=%s auto_init_wifi=%s\n",
               cfg.wifi_ssid,
               cfg.wifi_roam ? "true" : "false",
               cfg.auto_init_wifi ? "true" : "false");
    }

    if (do_reboot) {
        printf("Rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
    } else {
        printf("Reboot recommended. Run `config_clear_all reboot` for a fully fresh startup.\n");
    }
    return 0;
}

static int cmd_reboot_now(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
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

static int cmd_lcd_reinit(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = eldra_display_round_reset_panel();
    printf("lcd_reinit result: %s\n", esp_err_to_name(err));
    return 0;
}

static int cmd_lcd_sync(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = eldra_display_round_sync_panel_state();
    printf("lcd_sync result: %s\n", esp_err_to_name(err));
    return 0;
}

static int cmd_display_state(int argc, char **argv)
{
    (void)argc; (void)argv;
    int req_x = 0, req_y = 0;
    int eff_x = 0, eff_y = 0;
    int render_x = 0, render_y = 0;
    int disp_x = 0, disp_y = 0;
    eldra_eyes_get_center_offset(&req_x, &req_y);
    eldra_eyes_get_effective_center_offset(&eff_x, &eff_y);
    eldra_eyes_get_last_render_center_offset(&render_x, &render_y);
    eldra_eyes_get_display_center_offset(&disp_x, &disp_y);

    eldra_display_round_diag_t d = {0};
    eldra_display_round_get_diag(&d);

    printf("Display state:\n");
    printf("  eye_req=(%d,%d) eye_eff=(%d,%d) eye_render=(%d,%d) disp=(%d,%d)\n",
           req_x, req_y, eff_x, eff_y, render_x, render_y, disp_x, disp_y);
    printf("  panel: ready=%d sleeping=%d reinit=%d ready_t=%lu\n",
           d.panel_ready ? 1 : 0, d.panel_sleeping ? 1 : 0, d.reinit_in_progress ? 1 : 0,
           (unsigned long)d.panel_ready_stamp_ms);
    printf("  counters: reset=%lu sync=%lu sleep_enter=%lu sleep_exit=%lu\n",
           (unsigned long)d.reset_count, (unsigned long)d.sync_count,
           (unsigned long)d.sleep_enter_count, (unsigned long)d.sleep_exit_count);
    printf("  blit: ok=%lu fail=%lu timeout=%lu last_ok_t=%lu last_fail_t=%lu last_err=%s\n",
           (unsigned long)d.blit_ok_count, (unsigned long)d.blit_fail_count, (unsigned long)d.blit_timeout_count,
           (unsigned long)d.last_blit_ok_ms, (unsigned long)d.last_blit_fail_ms,
           esp_err_to_name(d.last_blit_err));
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

static int cmd_display_shift(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: display_shift <dx> <dy>\n");
        return 0;
    }
    int dx = (int)strtol(argv[1], NULL, 10);
    int dy = (int)strtol(argv[2], NULL, 10);
    eldra_display_round_set_shift(dx, dy);
    printf("Display shift is deprecated/disabled. Use disp_center/eyes_offset.\n");
    return 0;
}

static void display_diag_task(void *arg)
{
    (void)arg;
    printf("[diag] display_testpattern start\n");
    esp_err_t r = eldra_display_round_testpattern();
    printf("[diag] testpattern: %s\n", esp_err_to_name(r));

    printf("[diag] backlight 0 -> 90\n");
    eldra_display_round_set_backlight(0);
    vTaskDelay(pdMS_TO_TICKS(250));
    eldra_display_round_set_backlight(90);
    vTaskDelay(pdMS_TO_TICKS(150));

    printf("[diag] panel sleep in\n");
    r = eldra_display_round_panel_sleep(true);
    printf("[diag] sleep in: %s\n", esp_err_to_name(r));
    vTaskDelay(pdMS_TO_TICKS(350));

    printf("[diag] panel sleep out\n");
    r = eldra_display_round_panel_sleep(false);
    printf("[diag] sleep out: %s\n", esp_err_to_name(r));
    vTaskDelay(pdMS_TO_TICKS(120));

    printf("[diag] reset_panel\n");
    r = eldra_display_round_reset_panel();
    printf("[diag] reset_panel: %s\n", esp_err_to_name(r));
    vTaskDelay(pdMS_TO_TICKS(120));

    printf("[diag] final testpattern\n");
    r = eldra_display_round_testpattern();
    printf("[diag] final testpattern: %s\n", esp_err_to_name(r));
    printf("[diag] done\n");
    vTaskDelete(NULL);
}

static int cmd_display_diag(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (xTaskCreate(display_diag_task, "display_diag", 4096, NULL, 4, NULL) != pdPASS) {
        printf("display_diag: task create failed\n");
    } else {
        printf("display_diag started\n");
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

static void ping_http_task(void *arg)
{
    ping_http_args_t *args = (ping_http_args_t *)arg;
    if (!args) {
        vTaskDelete(NULL);
        return;
    }

    printf("Ping HTTP GET %s ...\n", args->url);
    esp_http_client_config_t cfg = {
        .url = args->url,
        .timeout_ms = 3000,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        printf("Ping init failed\n");
        free(args);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && status > 0) {
        printf("Ping %s -> HTTP %d\n", args->url, status);
    } else {
        printf("Ping %s failed: %s (status=%d)\n", args->url, esp_err_to_name(err), status);
    }

    free(args);
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
        printf("Usage: ping <url|host[:port]>\n");
        return 0;
    }
    char url_buf[256];
    const char *in = argv[1];
    const char *url = in;
    if (strstr(in, "://") == NULL) {
        snprintf(url_buf, sizeof(url_buf), "http://%s", in);
        url = url_buf;
    }

    ping_http_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        printf("Ping task alloc failed\n");
        return 0;
    }
    strlcpy(args->url, url, sizeof(args->url));

    if (xTaskCreatePinnedToCore(ping_http_task, "ping_http", 4096, args, 3, NULL, 0) != pdPASS) {
        printf("Ping task create failed\n");
        free(args);
        return 0;
    }
    printf("Ping started for %s\n", args->url);
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
    eldra_eyes_set_center_offset(x, y);
    int eff_x = x, eff_y = y;
    eldra_eyes_get_effective_center_offset(&eff_x, &eff_y);
    config_store_t cfg;
    if (config_store_load(&cfg) != ESP_OK) {
        printf("Load config failed\n");
        return 0;
    }
    cfg.eyes_center_x_offset = eff_x;
    cfg.eyes_center_y_offset = eff_y;
    if (config_store_save(&cfg) != ESP_OK) {
        printf("Save config failed\n");
    } else {
        printf("Saved eyes offsets to config (requested x=%d y=%d, saved/effective x=%d y=%d)\n",
               x, y, eff_x, eff_y);
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

static int cmd_sleep_now(int argc, char **argv)
{
    (void)argc; (void)argv;
    eldra_sleep_sleep_now();
    printf("Sleep requested\n");
    return 0;
}

static int cmd_wake_now(int argc, char **argv)
{
    (void)argc; (void)argv;
    eldra_sleep_wake_now();
    printf("Wake requested\n");
    return 0;
}

static eldra_glyph_id_t glyph_from_str(const char *s)
{
    if (!s) return GLYPH_SLEEP;
    if (strcasecmp(s, "sleep") == 0) return GLYPH_SLEEP;
    return GLYPH_SLEEP;
}

static int cmd_glyph_show(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: glyph_show <name> [slot]\n");
        return 0;
    }
    int slot = (argc >= 3) ? atoi(argv[2]) : 0;
    eldra_glyph_id_t id = glyph_from_str(argv[1]);
    // Manual console show should persist until explicitly hidden.
    eldra_glyphs_force_show(id, slot);
    printf("Glyph '%s' forced on slot %d (use glyph_hide to clear)\n", argv[1], slot);
    return 0;
}

static int cmd_glyph_hide(int argc, char **argv)
{
    int slot = (argc >= 2) ? atoi(argv[1]) : -1;
    eldra_glyphs_hide(slot);
    printf("Glyph hidden%s\n", slot < 0 ? " (all)" : "");
    return 0;
}

static int cmd_glyph_offset(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: glyph_offset <x> [y] [slot]\n");
        return 0;
    }
    int dx = (int)strtol(argv[1], NULL, 10);
    config_store_t cfg = {0};
    bool cfg_ok = (config_store_load(&cfg) == ESP_OK);
    int dy = cfg_ok ? cfg.glyph_offset_y : 0;
    if (argc >= 3) {
        dy = (int)strtol(argv[2], NULL, 10);
    }
    int slot = (argc >= 4) ? atoi(argv[3]) : 0;
    eldra_glyphs_set_offset(slot, dx, dy);
    if (slot == 0) {
        if (cfg_ok) {
            cfg.glyph_offset_x = dx;
            cfg.glyph_offset_y = dy;
            config_store_save(&cfg);
        }
    }
    printf("Glyph offset set x=%d y=%d slot=%d\n", dx, dy, slot);
    return 0;
}

static int cmd_glyph_scale(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: glyph_scale <scale>\n");
        return 0;
    }
    int sc = atoi(argv[1]);
    if (sc < 1) sc = 1;
    if (sc > 6) sc = 6;
    eldra_glyphs_set_scale(sc);
    config_store_t cfg;
    if (config_store_load(&cfg) == ESP_OK) {
        cfg.glyph_scale = sc;
        config_store_save(&cfg);
    }
    printf("Glyph scale set to %d\n", sc);
    return 0;
}

static int cmd_glyph_layout(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: glyph_layout <stack|ring>\n");
        return 0;
    }
    if (strcasecmp(argv[1], "ring") == 0) {
        eldra_glyphs_set_layout(GLYPH_LAYOUT_RING);
        printf("Glyph layout set to ring\n");
    } else {
        eldra_glyphs_set_layout(GLYPH_LAYOUT_STACK);
        printf("Glyph layout set to stack\n");
    }
    return 0;
}

static int cmd_sleep_window(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: sleep_window <start_hour> <end_hour>\n");
        return 0;
    }
    int start_h = (int)strtol(argv[1], NULL, 10);
    int end_h = (int)strtol(argv[2], NULL, 10);
    if (start_h < 0 || start_h > 23 || end_h < 0 || end_h > 23) {
        printf("Hours must be 0-23\n");
        return 0;
    }
    config_store_t cfg;
    if (config_store_load(&cfg) != ESP_OK) {
        printf("Load config failed\n");
        return 0;
    }
    cfg.sleep_start_hour = start_h;
    cfg.sleep_end_hour = end_h;
    if (config_store_save(&cfg) != ESP_OK) {
        printf("Save config failed\n");
    } else {
        printf("Sleep window saved start=%d end=%d\n", start_h, end_h);
    }
    emotion_set_sleep_window((uint8_t)start_h, (uint8_t)end_h);
    eldra_sleep_set_window((uint8_t)start_h, (uint8_t)end_h);
    return 0;
}

static int cmd_sleep_thresholds(int argc, char **argv)
{
    if (argc < 5) {
        printf("Usage: sleep_thresholds <window_idle_min> <low_battery_pct> <global_idle_min> <overfed_hold_min>\n");
        return 0;
    }

    int window_idle_min = (int)strtol(argv[1], NULL, 10);
    int low_battery_pct = (int)strtol(argv[2], NULL, 10);
    int global_idle_min = (int)strtol(argv[3], NULL, 10);
    int overfed_hold_min = (int)strtol(argv[4], NULL, 10);

    if (window_idle_min < 1 || global_idle_min < 1 || overfed_hold_min < 1 ||
        low_battery_pct < 1 || low_battery_pct > 100) {
        printf("Invalid thresholds: mins must be >=1, battery 1-100\n");
        return 0;
    }

    config_store_t cfg;
    if (config_store_load(&cfg) != ESP_OK) {
        printf("Load config failed\n");
        return 0;
    }

    cfg.sleep_window_inactivity_ms = window_idle_min * 60 * 1000;
    cfg.sleep_low_battery_pct = low_battery_pct;
    cfg.sleep_global_inactivity_ms = global_idle_min * 60 * 1000;
    cfg.sleep_overfed_hold_ms = overfed_hold_min * 60 * 1000;

    if (config_store_save(&cfg) != ESP_OK) {
        printf("Save config failed\n");
        return 0;
    }

    eldra_sleep_set_thresholds((uint32_t)cfg.sleep_window_inactivity_ms,
                               (uint8_t)cfg.sleep_low_battery_pct,
                               (uint32_t)cfg.sleep_global_inactivity_ms,
                               (uint32_t)cfg.sleep_overfed_hold_ms);
    printf("Sleep thresholds saved: window=%dmin low_batt=%d%% global=%dmin overfed=%dmin\n",
           window_idle_min, low_battery_pct, global_idle_min, overfed_hold_min);
    return 0;
}

static int cmd_angry_policy(int argc, char **argv)
{
    if (argc < 5) {
        printf("Usage: angry_policy <dizzy_count_threshold> <dizzy_window_min> <hold_min_min> <hold_max_min>\n");
        return 0;
    }

    int dizzy_count_threshold = (int)strtol(argv[1], NULL, 10);
    int dizzy_window_min = (int)strtol(argv[2], NULL, 10);
    int hold_min_min = (int)strtol(argv[3], NULL, 10);
    int hold_max_min = (int)strtol(argv[4], NULL, 10);

    if (dizzy_count_threshold < 1 || dizzy_window_min < 1 ||
        hold_min_min < 1 || hold_max_min < hold_min_min) {
        printf("Invalid values: threshold>=1, window>=1min, hold_min>=1min, hold_max>=hold_min\n");
        return 0;
    }

    config_store_t cfg;
    if (config_store_load(&cfg) != ESP_OK) {
        printf("Load config failed\n");
        return 0;
    }

    cfg.angry_dizzy_count_threshold = dizzy_count_threshold;
    cfg.angry_dizzy_window_ms = dizzy_window_min * 60 * 1000;
    cfg.angry_override_min_ms = hold_min_min * 60 * 1000;
    cfg.angry_override_max_ms = hold_max_min * 60 * 1000;

    if (config_store_save(&cfg) != ESP_OK) {
        printf("Save config failed\n");
        return 0;
    }

    emotion_set_angry_policy((uint8_t)cfg.angry_dizzy_count_threshold,
                             (uint32_t)cfg.angry_dizzy_window_ms,
                             (uint32_t)cfg.angry_override_min_ms,
                             (uint32_t)cfg.angry_override_max_ms);

    printf("Angry policy saved: dizzy_count>%d window=%dmin hold=%d..%dmin\n",
           dizzy_count_threshold, dizzy_window_min, hold_min_min, hold_max_min);
    return 0;
}

// Display test pattern (color bars)
static int cmd_display_testpattern(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t r = eldra_display_round_testpattern();
    printf("display_testpattern: %s\n", esp_err_to_name(r));
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

    const esp_console_cmd_t cfg_clear_cmd = {
        .command = "config_clear_all",
        .help = "Clear all persisted presets (SD+NVS) and restore defaults. Usage: config_clear_all [preserve_wifi] [reboot]",
        .hint = NULL,
        .func = &cmd_config_clear_all,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cfg_clear_cmd), TAG, "register config_clear_all failed");

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Reboot device immediately",
        .hint = NULL,
        .func = &cmd_reboot_now,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&reboot_cmd), TAG, "register reboot failed");

    const esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Alias for reboot",
        .hint = NULL,
        .func = &cmd_reboot_now,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&restart_cmd), TAG, "register restart failed");

    const esp_console_cmd_t fmt_cmd = {
        .command = "sd_format",
        .help = "Format SD card (destructive). Usage: sd_format yes",
        .hint = NULL,
        .func = &cmd_sd_format,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&fmt_cmd), TAG, "register sd_format failed");

    const esp_console_cmd_t lcd_reinit_cmd = {
        .command = "lcd_reinit",
        .help = "Re-run ST7701S init + panel reset (recover alignment/artifacts)",
        .hint = NULL,
        .func = &cmd_lcd_reinit,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&lcd_reinit_cmd), TAG, "register lcd_reinit failed");

    const esp_console_cmd_t lcd_sync_cmd = {
        .command = "lcd_sync",
        .help = "Re-assert panel scan/orientation state without full reinit",
        .hint = NULL,
        .func = &cmd_lcd_sync,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&lcd_sync_cmd), TAG, "register lcd_sync failed");

    const esp_console_cmd_t display_state_cmd = {
        .command = "display_state",
        .help = "Show eye offsets + display runtime diagnostics counters",
        .hint = NULL,
        .func = &cmd_display_state,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&display_state_cmd), TAG, "register display_state failed");

    // Display test pattern (color bars)
    const esp_console_cmd_t disp_test_cmd = {
        .command = "display_testpattern",
        .help = "Draw RGB color bars to the panel (debug)",
        .hint = NULL,
        .func = &cmd_display_testpattern,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&disp_test_cmd), TAG, "register display_testpattern failed");

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

    const esp_console_cmd_t loglevel_cmd = {
        .command = "loglevel",
        .help = "Set global console log level. Usage: loglevel <DEBUG|INFO|WARN|ERROR>",
        .hint = NULL,
        .func = &cmd_log_level,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&loglevel_cmd), TAG, "register loglevel failed");

    const esp_console_cmd_t logtag_cmd = {
        .command = "logtag",
        .help = "Per-tag console rule. Usage: logtag <tag> <on|off> [DEBUG|INFO|WARN|ERROR]",
        .hint = NULL,
        .func = &cmd_log_tag,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&logtag_cmd), TAG, "register logtag failed");

    const esp_console_cmd_t logtag_clear_cmd = {
        .command = "logtag_clear",
        .help = "Clear per-tag console rule(s). Usage: logtag_clear [tag]",
        .hint = NULL,
        .func = &cmd_log_tag_clear,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&logtag_clear_cmd), TAG, "register logtag_clear failed");

    const esp_console_cmd_t logtags_cmd = {
        .command = "logtags",
        .help = "List discovered log tags and console routing state. Usage: logtags [limit]",
        .hint = NULL,
        .func = &cmd_log_tags,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&logtags_cmd), TAG, "register logtags failed");

    const esp_console_cmd_t logfocus_cmd = {
        .command = "logfocus",
        .help = "Focus logs by subsystem. Usage: logfocus <off|scaffold|display|sleep|cloud|wifi|sensors|storage>",
        .hint = NULL,
        .func = &cmd_log_focus,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&logfocus_cmd), TAG, "register logfocus failed");

    const esp_console_cmd_t bridge_stats_cmd = {
        .command = "bridge_stats",
        .help = "Show scaffold/bridge queue depth and drop counters",
        .hint = NULL,
        .func = &cmd_bridge_stats,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&bridge_stats_cmd), TAG, "register bridge_stats failed");

    const esp_console_cmd_t monitor_status_cmd = {
        .command = "monitor_status",
        .help = "Show runtime monitor config + last performance snapshot stats",
        .hint = NULL,
        .func = &cmd_monitor_status,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&monitor_status_cmd), TAG, "register monitor_status failed");

    const esp_console_cmd_t monitor_cfg_cmd = {
        .command = "monitor_cfg",
        .help = "Set runtime monitor policy. Usage: monitor_cfg <snapshot_ms> [stall_ms] [hits] [auto_restart 0|1] [console_snap 0|1] [sd_snap 0|1]",
        .hint = NULL,
        .func = &cmd_monitor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&monitor_cfg_cmd), TAG, "register monitor_cfg failed");

    const esp_console_cmd_t disp_shift_cmd = {
        .command = "display_shift",
        .help = "Shift panel blit origin (debug). Usage: display_shift <dx> <dy>",
        .hint = NULL,
        .func = &cmd_display_shift,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&disp_shift_cmd), TAG, "register display_shift failed");

    const esp_console_cmd_t disp_diag_cmd = {
        .command = "display_diag",
        .help = "Run display bring-up diagnostics (pattern, backlight, sleep in/out, reset, pattern)",
        .hint = NULL,
        .func = &cmd_display_diag,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&disp_diag_cmd), TAG, "register display_diag failed");

    const esp_console_cmd_t sleep_cmd = {
        .command = "sleep_now",
        .help = "Trigger sleep sequence",
        .hint = NULL,
        .func = &cmd_sleep_now,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&sleep_cmd), TAG, "register sleep_now failed");

    const esp_console_cmd_t wake_cmd = {
        .command = "wake_now",
        .help = "Cancel sleep and wake",
        .hint = NULL,
        .func = &cmd_wake_now,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&wake_cmd), TAG, "register wake_now failed");

    const esp_console_cmd_t glyph_show_cmd = {
        .command = "glyph_show",
        .help = "Show and pin a glyph until hidden. Usage: glyph_show <sleep> [slot]",
        .hint = NULL,
        .func = &cmd_glyph_show,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&glyph_show_cmd), TAG, "register glyph_show failed");

    const esp_console_cmd_t glyph_hide_cmd = {
        .command = "glyph_hide",
        .help = "Hide glyph",
        .hint = NULL,
        .func = &cmd_glyph_hide,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&glyph_hide_cmd), TAG, "register glyph_hide failed");

    const esp_console_cmd_t glyph_offset_cmd = {
        .command = "glyph_offset",
        .help = "Set glyph offset. Usage: glyph_offset <x> [y] [slot]",
        .hint = NULL,
        .func = &cmd_glyph_offset,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&glyph_offset_cmd), TAG, "register glyph_offset failed");
    const esp_console_cmd_t glyph_scale_cmd = {
        .command = "glyph_scale",
        .help = "Set glyph scale. Usage: glyph_scale <scale>",
        .hint = NULL,
        .func = &cmd_glyph_scale,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&glyph_scale_cmd), TAG, "register glyph_scale failed");
    const esp_console_cmd_t glyph_layout_cmd = {
        .command = "glyph_layout",
        .help = "Set glyph layout. Usage: glyph_layout <stack|ring>",
        .hint = NULL,
        .func = &cmd_glyph_layout,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&glyph_layout_cmd), TAG, "register glyph_layout failed");

    const esp_console_cmd_t sleep_window_cmd = {
        .command = "sleep_window",
        .help = "Set sleep start/end hours (0-23). Usage: sleep_window <start> <end>",
        .hint = NULL,
        .func = &cmd_sleep_window,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&sleep_window_cmd), TAG, "register sleep_window failed");

    const esp_console_cmd_t sleep_thresholds_cmd = {
        .command = "sleep_thresholds",
        .help = "Set sleep thresholds. Usage: sleep_thresholds <window_idle_min> <low_battery_pct> <global_idle_min> <overfed_hold_min>",
        .hint = NULL,
        .func = &cmd_sleep_thresholds,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&sleep_thresholds_cmd), TAG, "register sleep_thresholds failed");

    const esp_console_cmd_t angry_policy_cmd = {
        .command = "angry_policy",
        .help = "Set angry escalation policy. Usage: angry_policy <dizzy_count_threshold> <dizzy_window_min> <hold_min_min> <hold_max_min>",
        .hint = NULL,
        .func = &cmd_angry_policy,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&angry_policy_cmd), TAG, "register angry_policy failed");

    EL_LOGI(TAG, "SD console commands ready");
    return ESP_OK;
}

