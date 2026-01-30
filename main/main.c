#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "eldra_display_round.h"
#include "eldra_glyphs.h"
#include "eldra_logging.h"
#include "eldra_eyes.h"
#include "eldra_sensors.h"
#include "eldra_emotion.h"
#include "eldra_comms.h"
#include "eldra_cloud.h"
#include "eldra_sleep.h"

#include "console_app.h"
#include "console_wifi.h"
#include "console_sd.h"
#include "console_rtc.h"
#include "console_imu.h"
#include "console_emotion.h"

#include "wifi_driver.h"
#include "sd_driver.h"
#include "config_store.h"
#include "imu_qmi8658.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_main";
static eldra_eyes_context_t *g_eyes_ctx = NULL;
static emotion_context_t g_emotion = {0};
static bool g_emotion_ready = false;
static bool g_wifi_online = false;
static bool g_cloud_ready = false;
static uint64_t g_last_cloud_state_push_ms = 0;
static bool sntp_started = false;
static bool g_has_ip = false;
static uint64_t g_last_mood_save_ms = 0;

typedef struct {
    char ssid[33];
    char pass[65];
    bool roam;
} auto_wifi_cfg_t;
static auto_wifi_cfg_t s_auto_wifi_cfg = {0};
static const bool k_auto_calibrate_on_boot = false;
static const uint32_t k_heartbeat_interval_ms = 5000;
static const uint32_t k_wdt_timeout_seconds = 8;
static const bool k_enable_heartbeat = false;
static const uint32_t k_mood_save_interval_ms = 300000; // 5 minutes

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Scan the framebuffer for non-background pixels and return the bounding box.
static bool measure_frame_bounds(const uint16_t *fb, int w, int h,
                                 int *min_x, int *max_x, int *min_y, int *max_y)
{
    if (!fb || w <= 0 || h <= 0) return false;
    uint16_t bg = fb[0];
    int lx = w, rx = -1, ty = h, by = -1;
    for (int y = 0; y < h; ++y) {
        const uint16_t *row = fb + ((size_t)y * (size_t)w);
        for (int x = 0; x < w; ++x) {
            if (row[x] != bg) {
                if (x < lx) lx = x;
                if (x > rx) rx = x;
                if (y < ty) ty = y;
                if (y > by) by = y;
            }
        }
    }
    if (rx < 0 || by < 0) return false;
    if (min_x) *min_x = lx;
    if (max_x) *max_x = rx;
    if (min_y) *min_y = ty;
    if (max_y) *max_y = by;
    return true;
}

// Render once and auto-correct center offsets if content is off-center. Optionally persists.
static void auto_calibrate_eyes(eldra_eyes_context_t *eyes_ctx,
                                uint16_t *fb, int w, int h,
                                config_store_t *cfg, bool cfg_loaded)
{
    if (!eyes_ctx || !fb) return;
    eldra_eyes_render(eyes_ctx, fb, (uint16_t)w, (uint16_t)h);
    int minx, maxx, miny, maxy;
    if (!measure_frame_bounds(fb, w, h, &minx, &maxx, &miny, &maxy)) {
        EL_LOGW(TAG, "Auto-calibrate: no content detected");
        return;
    }
    int cx = (minx + maxx) / 2;
    int cy = (miny + maxy) / 2;
    int target_x = w / 2;
    int target_y = h / 2;
    int dx = target_x - cx;
    int dy = target_y - cy;
    if (dx == 0 && dy == 0) {
        EL_LOGI(TAG, "Auto-calibrate: already centered");
        return;
    }
    int new_x = cfg ? cfg->eyes_center_x_offset + dx : dx;
    int new_y = cfg ? cfg->eyes_center_y_offset + dy : dy;
    eldra_eyes_set_center_offset(new_x, new_y);
    EL_LOGI(TAG, "Auto-calibrate: applied dx=%d dy=%d -> offsets x=%d y=%d", dx, dy, new_x, new_y);
    if (cfg && cfg_loaded) {
        cfg->eyes_center_x_offset = new_x;
        cfg->eyes_center_y_offset = new_y;
        (void)config_store_save(cfg);
    }
}

static const char *emotion_state_str(emotion_state_t st)
{
    switch (st) {
        case EMOTION_STATE_NEUTRAL: return "NEUTRAL";
        case EMOTION_STATE_HAPPY: return "HAPPY";
        case EMOTION_STATE_SAD: return "SAD";
        case EMOTION_STATE_LONELY: return "LONELY";
        case EMOTION_STATE_SLEEPY: return "SLEEPY";
        case EMOTION_STATE_HUNGRY: return "HUNGRY";
        case EMOTION_STATE_PLAYFUL: return "PLAYFUL";
        case EMOTION_STATE_ELDRITCH: return "ELDRITCH";
        case EMOTION_STATE_SCARED: return "SCARED";
        case EMOTION_STATE_DIZZY: return "DIZZY";
        default: return "UNKNOWN";
    }
}

static eldra_eyes_mood_t mood_for_state(emotion_state_t st)
{
    switch (st) {
        case EMOTION_STATE_HAPPY: return ELDRA_EYES_MOOD_HAPPY;
        case EMOTION_STATE_SAD: return ELDRA_EYES_MOOD_SAD;
        case EMOTION_STATE_LONELY: return ELDRA_EYES_MOOD_BORED;
        case EMOTION_STATE_SLEEPY: return ELDRA_EYES_MOOD_SLEEPY;
        case EMOTION_STATE_HUNGRY: return ELDRA_EYES_MOOD_HUNGRY;
        case EMOTION_STATE_ELDRITCH: return ELDRA_EYES_MOOD_ELDRITCH_RUNE;
        case EMOTION_STATE_SCARED: return ELDRA_EYES_MOOD_ANGRY;
        default: return ELDRA_EYES_MOOD_NEUTRAL;
    }
}

static void imu_callback(float gx_dps, float gy_dps, float gz_dps,
                         float ax_g, float ay_g, float az_g, uint32_t dt_ms) {
    if (g_eyes_ctx) {
        eldra_eyes_handle_imu(g_eyes_ctx, gx_dps, gy_dps, gz_dps, ax_g, ay_g, az_g, dt_ms);
    }
}

static void auto_wifi_task(void *arg)
{
    (void)arg;
    const int max_attempts = 3;
    const TickType_t attempt_delay = pdMS_TO_TICKS(20000); // 20s between attempts
    wifi_driver_status_t st = WIFI_STATUS_IDLE;
    wifi_ap_record_t ap = {0};

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        esp_err_t w = wifi_driver_connect_best_async(s_auto_wifi_cfg.ssid, s_auto_wifi_cfg.pass);
        if (w == ESP_OK) {
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "Auto WiFi attempt %d started", attempt);
            sd_driver_log_async(logbuf);
            EL_LOGI(TAG, "%s", logbuf);
        } else {
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "Auto WiFi attempt %d failed to start: %s", attempt, esp_err_to_name(w));
            sd_driver_log_async(logbuf);
            EL_LOGW(TAG, "%s", logbuf);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
        wifi_driver_get_status(&st, &ap);
        EL_LOGI(TAG, "Auto WiFi attempt %d status=%d RSSI=%d", attempt, st, ap.rssi);
        if (st == WIFI_STATUS_CONNECTED) {
            sd_driver_log_async("Auto WiFi connected");
            EL_LOGI(TAG, "WiFi connected to \"%s\" RSSI=%d", ap.ssid, ap.rssi);
            vTaskDelete(NULL);
            return;
        }
        if (attempt < max_attempts) {
            vTaskDelay(attempt_delay);
        }
    }
    sd_driver_log_async("Auto WiFi: all retries exhausted");
    EL_LOGW(TAG, "Auto WiFi retries exhausted");
    vTaskDelete(NULL);
}

static void got_ip_start_sntp(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (sntp_started) {
        return;
    }
    sntp_started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    sd_driver_log_async("SNTP started (EST5EDT)");
    printf("SNTP started for timezone EST5EDT\n");
}

static void on_ip_acquired(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    g_has_ip = true;
    g_wifi_online = true;
    EL_LOGI(TAG, "Got IP: " IPSTR ", mask " IPSTR ", gw " IPSTR,
            IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));
    if (g_cloud_ready) {
        eldra_cloud_set_online(true);
    }
}

static void on_wifi_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    g_has_ip = false;
    g_wifi_online = false;
    EL_LOGW(TAG, "WiFi disconnected");
    if (g_cloud_ready) {
        eldra_cloud_set_online(false);
    }
}

static void handle_cloud_command(const eldra_command_t *cmd)
{
    if (!cmd) {
        return;
    }
    pet_command_t pcmd = {0};
    switch (cmd->type) {
        case ELDRA_CMD_TYPE_FEED: pcmd.type = CMD_FEED; pcmd.arg0 = (uint32_t)cmd->arg0; break;
        case ELDRA_CMD_TYPE_PET: pcmd.type = CMD_PET; break;
        case ELDRA_CMD_TYPE_PLAY: pcmd.type = CMD_PLAY; break;
        case ELDRA_CMD_TYPE_DEBUG_FORCE_STATE: pcmd.type = CMD_DEBUG_FORCE_STATE; pcmd.arg0 = (uint32_t)cmd->arg0; break;
        case ELDRA_CMD_TYPE_SET_FLAG: pcmd.type = CMD_SET_FLAG; pcmd.arg0 = (uint32_t)cmd->arg0; pcmd.arg1 = (uint32_t)cmd->arg1; break;
        case ELDRA_CMD_TYPE_RFID_ITEM: pcmd.type = CMD_RESERVED_RFID; pcmd.arg0 = (uint32_t)cmd->arg0; break;
        case ELDRA_CMD_TYPE_SERVER_SCRIPTED_EVENT: pcmd.type = CMD_RESERVED_SERVER; pcmd.arg0 = (uint32_t)cmd->arg0; break;
        default: pcmd.type = CMD_RESERVED_SERVER; break;
    }
    bool ok = comms_enqueue_command(&pcmd);
    eldra_cloud_ack_command(cmd, ok, ok ? NULL : "queue full");
}

static void push_cloud_state_if_ready(uint32_t now_ms)
{
    if (!g_cloud_ready) {
        return;
    }
    if (now_ms - (uint32_t)g_last_cloud_state_push_ms < 1000U) {
        return;
    }
    g_last_cloud_state_push_ms = now_ms;

    eldra_state_t st = {0};
    st.emotion_state = emotion_state_str(g_emotion.current_state);
    st.happiness = g_emotion.happiness;
    st.hunger = g_emotion.hunger;
    st.energy = g_emotion.energy;
    st.social = g_emotion.social;
    st.fear = g_emotion.fear;
    st.eldritch_charge = g_emotion.eldritch_charge;
    st.battery_percentage = (int)eldra_sensors_get_battery_percent();
    st.battery_voltage_mv = (int)(eldra_sensors_get_battery_voltage() * 1000.0f);
    st.battery_is_charging = false;
    st.flag_asleep = false;
    st.flag_low_power = false;
    st.flag_debug_mode = false;
    eldra_cloud_set_state(&st);
}

static void apply_saved_mood(const config_store_t *cfg)
{
    if (!cfg) return;
    if (cfg->mood_happiness >= 0) g_emotion.happiness = (uint8_t)clamp_int(cfg->mood_happiness, 0, 100);
    if (cfg->mood_hunger >= 0) g_emotion.hunger = (uint8_t)clamp_int(cfg->mood_hunger, 0, 130);
    if (cfg->mood_energy >= 0) g_emotion.energy = (uint8_t)clamp_int(cfg->mood_energy, 0, 100);
    if (cfg->mood_social >= 0) g_emotion.social = (uint8_t)clamp_int(cfg->mood_social, 0, 100);
    if (cfg->mood_fear >= 0) g_emotion.fear = (uint8_t)clamp_int(cfg->mood_fear, 0, 100);
    if (cfg->mood_eldritch_charge >= 0) g_emotion.eldritch_charge = (uint8_t)clamp_int(cfg->mood_eldritch_charge, 0, 100);
    if (cfg->mood_state >= 0 && cfg->mood_state <= EMOTION_STATE_DIZZY) {
        g_emotion.current_state = (emotion_state_t)cfg->mood_state;
    }
}

static void save_mood_to_config(void)
{
    config_store_t cfg;
    if (config_store_load(&cfg) != ESP_OK) {
        return;
    }
    cfg.mood_happiness = g_emotion.happiness;
    cfg.mood_hunger = g_emotion.hunger;
    cfg.mood_energy = g_emotion.energy;
    cfg.mood_social = g_emotion.social;
    cfg.mood_fear = g_emotion.fear;
    cfg.mood_eldritch_charge = g_emotion.eldritch_charge;
    cfg.mood_state = (int)g_emotion.current_state;
    (void)config_store_save(&cfg);
}

void app_main(void) {
    log_init();
    log_set_console_level(LOG_LEVEL_INFO);
    esp_log_level_set("*", ESP_LOG_INFO);
    EL_LOGI(TAG, "app_main start");

    if (eldra_sensors_init() != ESP_OK) {
        EL_LOGE(TAG, "Sensor init failed; holding");
        goto fail_safe;
    }
    EL_LOGI(TAG, "Sensors init complete");

    // Small settle to let rails stabilize before the LCD pulls current.
    vTaskDelay(pdMS_TO_TICKS(200));

    if (eldra_display_round_init() != ESP_OK) {
        EL_LOGE(TAG, "Display init failed; holding");
        goto fail_safe;
    }
    eldra_display_round_set_backlight(90);

    comms_init();
    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    emotion_init(&g_emotion, start_ms);
    g_emotion_ready = true;

    if (ConsoleEmotion_Init(&g_emotion) != ESP_OK) {
        EL_LOGW(TAG, "Emotion console init failed");
    }

    if (Console_Init() != ESP_OK) {
        EL_LOGW(TAG, "Console init failed");
    }
    if (ConsoleWiFi_Init() != ESP_OK) {
        EL_LOGW(TAG, "WiFi console init failed");
    }
    if (ConsoleSD_Init() != ESP_OK) {
        EL_LOGW(TAG, "SD console init failed");
    }
    if (ConsoleRTC_Init() != ESP_OK) {
        EL_LOGW(TAG, "RTC console init failed");
    }
    if (ConsoleIMU_Init() != ESP_OK) {
        EL_LOGW(TAG, "IMU console init failed");
    }

    // Enable a task watchdog to catch hard hangs; feed it in the main loop below.
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = k_wdt_timeout_seconds * 1000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true,
    };
    esp_err_t wdt_err = esp_task_wdt_init(&wdt_cfg);
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_INVALID_STATE) {
        EL_LOGW(TAG, "WDT init failed: %s", esp_err_to_name(wdt_err));
    }
    esp_err_t add_err = esp_task_wdt_add(NULL);
    if (add_err != ESP_OK && add_err != ESP_ERR_INVALID_STATE) {
        EL_LOGW(TAG, "WDT add failed: %s", esp_err_to_name(add_err));
    }

    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        EL_LOGW(TAG, "netif init failed: %s", esp_err_to_name(e));
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        EL_LOGW(TAG, "event loop init failed: %s", esp_err_to_name(e));
    }
    (void)esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_acquired, NULL);
    (void)esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_disconnect, NULL);
    // Optional: also start SNTP when IP arrives.
    (void)esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip_start_sntp, NULL);

    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    static esp_event_handler_instance_t ip_handler;
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        got_ip_start_sntp, NULL, &ip_handler);

    config_store_t cfg = {0};
    bool cfg_loaded = false;
    if (sd_driver_init() == ESP_OK) {
        log_sd_notify_mounted();
        if (config_store_load(&cfg) == ESP_OK) {
            cfg_loaded = true;
        } else {
            EL_LOGW(TAG, "Config load failed");
        }
    } else {
        EL_LOGW(TAG, "SD init failed; skipping config load");
    }
    if (!cfg_loaded) {
        config_store_get_defaults(&cfg);
    }
    // Apply configurable sleep window and mood log interval to the emotion engine.
    emotion_set_sleep_window((uint8_t)cfg.sleep_start_hour, (uint8_t)cfg.sleep_end_hour);
    emotion_set_mood_log_interval_minutes((uint32_t)cfg.mood_log_interval_minutes);
    apply_saved_mood(&cfg);
    g_last_mood_save_ms = (uint64_t)start_ms;

    if (cfg_loaded) {
        if (cfg.wifi_ssid[0] == '\0') {
            FILE *wf = fopen("/sdcard/WIFI/WIFI.CFG", "r");
            if (wf) {
                char line[96];
                while (fgets(line, sizeof(line), wf)) {
                    if (strncmp(line, "ssid=", 5) == 0) {
                        strlcpy(cfg.wifi_ssid, line + 5, sizeof(cfg.wifi_ssid));
                        cfg.wifi_ssid[strcspn(cfg.wifi_ssid, "\r\n")] = 0;
                    } else if (strncmp(line, "pass=", 5) == 0) {
                        strlcpy(cfg.wifi_pass, line + 5, sizeof(cfg.wifi_pass));
                        cfg.wifi_pass[strcspn(cfg.wifi_pass, "\r\n")] = 0;
                    }
                }
                fclose(wf);
            }
        }

        if (cfg.auto_init_wifi) {
            if (cfg.wifi_ssid[0] != '\0') {
                wifi_driver_set_roaming(cfg.wifi_roam);
                strlcpy(s_auto_wifi_cfg.ssid, cfg.wifi_ssid, sizeof(s_auto_wifi_cfg.ssid));
                strlcpy(s_auto_wifi_cfg.pass, cfg.wifi_pass, sizeof(s_auto_wifi_cfg.pass));
                s_auto_wifi_cfg.roam = cfg.wifi_roam;
                if (xTaskCreatePinnedToCore(auto_wifi_task, "auto_wifi", 4096, NULL, 4, NULL, 0) != pdPASS) {
                    sd_driver_log_async("Auto WiFi task create failed");
                    EL_LOGE(TAG, "Auto WiFi: failed to create retry task");
                } else {
                    EL_LOGI(TAG, "Auto WiFi retry task started for \"%s\"", cfg.wifi_ssid);
                    sd_driver_log_async("Auto WiFi retry task started");
                }
            } else {
                EL_LOGW(TAG, "Auto WiFi enabled but no saved credentials");
                sd_driver_log_async("Auto WiFi: no saved credentials");
            }
        }

        if (cfg.auto_init_imu) {
            esp_err_t ir = qmi8658_init();
            if (ir == ESP_OK) {
                sd_driver_log_async("IMU auto-init OK");
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "IMU auto-init failed: %s", esp_err_to_name(ir));
                sd_driver_log_async(msg);
                EL_LOGW(TAG, "%s", msg);
            }
        }

        if (cfg.auto_init_cloud && cfg.cloud_base_url[0] != '\0' && cfg.cloud_token[0] != '\0') {
            eldra_cloud_set_intervals(cfg.cloud_poll_interval_ms, cfg.cloud_state_interval_ms, cfg.cloud_log_interval_ms);
            eldra_cloud_init(cfg.cloud_base_url, cfg.cloud_token);
            eldra_cloud_set_console_logging(cfg.cloud_logs_console);
            eldra_cloud_register_command_handler(handle_cloud_command);
            eldra_cloud_set_online(false);
            g_cloud_ready = true;
            // If WiFi already delivered an IP before cloud init completed, bring cloud online now.
            if (g_has_ip) {
                eldra_cloud_set_online(true);
            }
        }
    } else {
        EL_LOGW(TAG, "Config not loaded; auto-init skipped");
    }

    const int fb_width = eldra_display_round_get_width();
    const int fb_height = eldra_display_round_get_height();

    eldra_eyes_context_t *eyes_ctx = eldra_eyes_create();
    if (!eyes_ctx) {
        EL_LOGE(TAG, "Failed to create eyes context; holding");
        goto fail_safe;
    }
    // Apply persisted display/eye center offsets if available.
    eldra_eyes_set_display_center_offset(cfg.display_center_x_offset, cfg.display_center_y_offset);
    eldra_eyes_set_center_offset(cfg.eyes_center_x_offset, cfg.eyes_center_y_offset);
    g_eyes_ctx = eyes_ctx;
    eldra_sensors_set_imu_callback(imu_callback);

    size_t buf_size_bytes = (size_t)fb_width * (size_t)fb_height * sizeof(uint16_t);
    uint16_t *framebuffer = (uint16_t *)heap_caps_malloc(buf_size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!framebuffer) {
        framebuffer = (uint16_t *)heap_caps_malloc(buf_size_bytes, MALLOC_CAP_8BIT);
        if (!framebuffer) {
            EL_LOGE(TAG, "Framebuffer alloc failed; holding");
            goto fail_safe;
        }
    }

    for (size_t i = 0; i < (size_t)fb_width * (size_t)fb_height; ++i) {
        framebuffer[i] = 0xFFFF; // white
    }
    esp_err_t test_blit = eldra_display_round_blit(framebuffer, fb_width, fb_height);
    EL_LOGI(TAG, "Test pattern blit result=%d", test_blit);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (k_auto_calibrate_on_boot) {
        // Auto-calibrate eye centering once at boot using current offsets and persist if config is loaded.
        auto_calibrate_eyes(eyes_ctx, framebuffer, fb_width, fb_height, cfg_loaded ? &cfg : NULL, cfg_loaded);
    } else {
        EL_LOGI(TAG, "Auto-calibrate on boot disabled; use disp_center/eyes_offset then persist.");
    }

    eldra_sleep_init();

    uint64_t last_us = esp_timer_get_time();
    uint32_t last_heartbeat_ms = (uint32_t)(last_us / 1000ULL);
    while (1) {
        uint64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - last_us) / 1000ULL);
        if (dt_ms == 0) {
            dt_ms = 1;
        }
        last_us = now_us;

        uint32_t now_ms = (uint32_t)(now_us / 1000ULL);

        if (g_emotion_ready) {
            emotion_set_battery_percent(&g_emotion, eldra_sensors_get_battery_percent());
            emotion_on_tick(&g_emotion, now_ms);
            comms_process_all_pending(&g_emotion, now_ms);
            eldra_eyes_mood_t desired = mood_for_state(g_emotion.current_state);
            if (eldra_eyes_get_mood(eyes_ctx) != desired) {
                eldra_eyes_set_mood(eyes_ctx, desired);
            }
        }

        push_cloud_state_if_ready(now_ms);

        // Heartbeat every few seconds to SD/console to catch silent hangs.
        if (k_enable_heartbeat && (now_ms - last_heartbeat_ms) >= k_heartbeat_interval_ms) {
            last_heartbeat_ms = now_ms;
            char hb[64];
            snprintf(hb, sizeof(hb), "HB t=%lu wifi=%d cloud=%d", (unsigned long)now_ms, g_wifi_online, g_cloud_ready);
            sd_driver_log_async(hb);
            EL_LOGI(TAG, "%s", hb);
        }

        if ((now_ms - (uint32_t)g_last_mood_save_ms) >= k_mood_save_interval_ms) {
            save_mood_to_config();
            g_last_mood_save_ms = now_ms;
        }

        eldra_sleep_tick(now_ms);

        eldra_eyes_update(eyes_ctx, dt_ms);
        eldra_eyes_render(eyes_ctx, framebuffer, (uint16_t)fb_width, (uint16_t)fb_height);
        eldra_glyphs_render(framebuffer, fb_width, fb_height, now_ms);
        esp_err_t blit_ret = eldra_display_round_blit(framebuffer, fb_width, fb_height);
        if (blit_ret != ESP_OK) {
            EL_LOGE(TAG, "Blit failed: %d", blit_ret);
        }

        // Feed watchdog; if we hard hang before this point, WDT will panic and give us a backtrace.
        esp_task_wdt_reset();

        vTaskDelay(pdMS_TO_TICKS(16)); // ~60 FPS pacing
    }

fail_safe:
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
