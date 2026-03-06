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
#include "rtc_driver.h"
#include "imu_qmi8658.h"
#include "TCA9554PWR.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "app_main";
static eldra_eyes_context_t *g_eyes_ctx = NULL;
static emotion_context_t g_emotion = {0};
static bool g_emotion_ready = false;
static volatile bool g_wifi_online = false;
static bool g_cloud_ready = false;
static volatile bool g_force_render_now = false;
static volatile bool g_panel_ready_seen = false;
static volatile bool g_sleep_lid_heavy = false;
static uint64_t g_last_cloud_state_push_ms = 0;
static bool sntp_started = false;
static volatile bool g_has_ip = false;
static volatile bool g_net_state_dirty = false;
static uint64_t g_last_mood_save_ms = 0;
static uint32_t g_last_seen_mood_log_ms = 0;
static uint32_t g_prev_mood_log_ms = 0;
static volatile uint32_t g_display_blit_ok_count = 0;
static uint32_t g_prev_display_blit_ok_count = 0;
static uint64_t g_last_time_persist_ms = 0;
static SemaphoreHandle_t g_eyes_lock = NULL;
static TaskHandle_t g_display_task_handle = NULL;
static volatile bool g_sd_io_guard_ready = false;

typedef struct {
    float gx_dps;
    float gy_dps;
    float gz_dps;
    float ax_g;
    float ay_g;
    float az_g;
    uint32_t dt_ms;
    uint16_t coalesced_samples;
    bool pending;
} imu_pending_sample_t;

static portMUX_TYPE g_imu_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static imu_pending_sample_t g_imu_pending = {0};

typedef struct {
    char ssid[33];
    char pass[65];
    bool roam;
} auto_wifi_cfg_t;
static auto_wifi_cfg_t s_auto_wifi_cfg = {0};
static const bool k_auto_calibrate_on_boot = false;
static const uint32_t k_heartbeat_interval_ms = 5000;
static const bool k_enable_heartbeat = false;
static const uint32_t k_mood_save_interval_ms = 0; // disabled: runtime SD writes can disturb display
static const uint32_t k_time_persist_interval_ms = 0; // disabled: runtime RTC/NVS writes can disturb display
static const bool k_force_runtime_sd_logging_off = true; // diagnostic: eliminate SD writes during active runtime
static const uint32_t k_display_probe_interval_ms = 5000;
static const int k_display_probe_jump_px = 24;
static config_store_t g_cfg_current = {0};

typedef struct {
    uint16_t *framebuffer;
    int fb_width;
    int fb_height;
} display_runtime_t;
static display_runtime_t g_display_runtime = {0};
static bool measure_frame_bounds(const uint16_t *fb, int w, int h,
                                 int *min_x, int *max_x, int *min_y, int *max_y);
static volatile uint32_t g_imu_coalesce_events = 0;
static volatile uint32_t g_imu_coalesce_max_dt_ms = 0;

static const char *reset_reason_name(esp_reset_reason_t rr)
{
    switch (rr) {
        case ESP_RST_UNKNOWN: return "unknown";
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_EXT: return "external_pin";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default: return "unmapped";
    }
}

static inline bool eyes_lock_take(TickType_t ticks)
{
    return (g_eyes_lock == NULL) || (xSemaphoreTake(g_eyes_lock, ticks) == pdTRUE);
}

static inline void eyes_lock_give(void)
{
    if (g_eyes_lock) {
        xSemaphoreGive(g_eyes_lock);
    }
}

void eldra_platform_before_sd_io(void)
{
    if (!g_sd_io_guard_ready) {
        return;
    }
    EXIO_ForceLCDIdle();
}

void eldra_platform_after_sd_io(void)
{
    if (!g_sd_io_guard_ready) {
        return;
    }
    EXIO_ForceLCDIdle();
}

static inline void imu_pending_store(float gx_dps, float gy_dps, float gz_dps,
                                     float ax_g, float ay_g, float az_g, uint32_t dt_ms)
{
    uint32_t step_ms = (dt_ms == 0) ? 1 : dt_ms;
    portENTER_CRITICAL(&g_imu_pending_lock);
    if (g_imu_pending.pending) {
        if (g_imu_pending.coalesced_samples < UINT16_MAX) {
            g_imu_pending.coalesced_samples++;
        }
        if (g_imu_coalesce_events < UINT32_MAX) {
            g_imu_coalesce_events++;
        }
        uint32_t merged = g_imu_pending.dt_ms + step_ms;
        if (merged > 500U) {
            merged = 500U;
        }
        g_imu_pending.dt_ms = merged;
    } else {
        g_imu_pending.dt_ms = step_ms;
        g_imu_pending.coalesced_samples = 0;
    }
    g_imu_pending.gx_dps = gx_dps;
    g_imu_pending.gy_dps = gy_dps;
    g_imu_pending.gz_dps = gz_dps;
    g_imu_pending.ax_g = ax_g;
    g_imu_pending.ay_g = ay_g;
    g_imu_pending.az_g = az_g;
    if (g_imu_pending.dt_ms > g_imu_coalesce_max_dt_ms) {
        g_imu_coalesce_max_dt_ms = g_imu_pending.dt_ms;
    }
    g_imu_pending.pending = true;
    portEXIT_CRITICAL(&g_imu_pending_lock);
}

static inline bool imu_pending_take(imu_pending_sample_t *out)
{
    if (!out) {
        return false;
    }
    bool has_sample = false;
    portENTER_CRITICAL(&g_imu_pending_lock);
    if (g_imu_pending.pending) {
        *out = g_imu_pending;
        g_imu_pending.pending = false;
        has_sample = true;
    }
    portEXIT_CRITICAL(&g_imu_pending_lock);
    return has_sample;
}

static inline void compose_frame(uint16_t *fb, int w, int h, uint32_t now_ms) {
    if (!fb || w <= 0 || h <= 0) return;
    size_t buf_size_bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
    memset(fb, 0, buf_size_bytes);                // clear whole frame every tick
    eldra_eyes_render(g_eyes_ctx, fb, (uint16_t)w, (uint16_t)h);
    int eff_eye_dx = 0;
    int eff_eye_dy = 0;
    // Lock glyph placement to the exact clamped eye center used by this render.
    eldra_eyes_get_last_render_center_offset(&eff_eye_dx, &eff_eye_dy);
    eldra_glyphs_set_eye_center_offset(eff_eye_dx, eff_eye_dy);
    eldra_glyphs_render(fb, w, h, now_ms);        // layer glyphs atop eyes
}

static void display_task(void *arg)
{
    (void)arg;
    uint64_t last_us = esp_timer_get_time();
    uint32_t last_fail_log_ms = 0;
    uint32_t panel_not_ready_since_ms = 0;
    uint32_t last_auto_reset_ms = 0;
    uint32_t last_blit_ok_ms = (uint32_t)(last_us / 1000ULL);
    uint32_t last_blit_recover_ms = 0;
    uint32_t eyes_lock_miss_streak = 0;
    const int expected_w = eldra_display_round_get_width();
    const int expected_h = eldra_display_round_get_height();
    bool logged_dim_guard = false;
    int last_eye_dx = 0x7FFFFFFF;
    int last_eye_dy = 0x7FFFFFFF;
    uint32_t last_probe_ms = 0;
    int last_probe_cx = 0x7FFFFFFF;
    int last_probe_cy = 0x7FFFFFFF;
    uint32_t last_drift_warn_ms = 0;
    uint32_t last_seen_reset_count = 0;
    bool last_panel_sleeping = false;
    bool last_panel_reinit = false;
    bool last_sleep_active = false;
    uint8_t post_reset_probe_stage = 0;
    uint32_t post_reset_probe_next_ms = 0;

    while (1) {
        uint64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - last_us) / 1000ULL);
        if (dt_ms == 0) dt_ms = 1;
        last_us = now_us;
        uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
        bool sleep_active = eldra_sleep_is_active();

        if (sleep_active) {
            if (!last_sleep_active) {
                if (g_display_runtime.framebuffer &&
                    g_display_runtime.fb_width > 0 &&
                    g_display_runtime.fb_height > 0) {
                    size_t bytes = (size_t)g_display_runtime.fb_width *
                                   (size_t)g_display_runtime.fb_height *
                                   sizeof(uint16_t);
                    memset(g_display_runtime.framebuffer, 0, bytes);
                    if (eldra_display_round_is_ready()) {
                        (void)eldra_display_round_blit(g_display_runtime.framebuffer,
                                                       g_display_runtime.fb_width,
                                                       g_display_runtime.fb_height);
                    }
                }
            }
            last_sleep_active = true;
            last_us = now_us;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (last_sleep_active) {
            last_sleep_active = false;
            last_us = now_us;
            g_force_render_now = true;
        }

        if (g_display_runtime.fb_width != expected_w || g_display_runtime.fb_height != expected_h) {
            if (!logged_dim_guard) {
                EL_LOGE(TAG, "Display runtime dims drifted (%d,%d) expected (%d,%d); restoring",
                        g_display_runtime.fb_width, g_display_runtime.fb_height, expected_w, expected_h);
                logged_dim_guard = true;
            }
            g_display_runtime.fb_width = expected_w;
            g_display_runtime.fb_height = expected_h;
        } else {
            logged_dim_guard = false;
        }

        if (g_eyes_ctx && g_display_runtime.framebuffer) {
            if (eyes_lock_take(pdMS_TO_TICKS(2))) {
                imu_pending_sample_t imu_sample = {0};
                if (imu_pending_take(&imu_sample)) {
                    eldra_eyes_handle_imu(g_eyes_ctx,
                                          imu_sample.gx_dps,
                                          imu_sample.gy_dps,
                                          imu_sample.gz_dps,
                                          imu_sample.ax_g,
                                          imu_sample.ay_g,
                                          imu_sample.az_g,
                                          imu_sample.dt_ms);
                }
                eldra_eyes_set_sleep_lid_heavy(g_eyes_ctx, g_sleep_lid_heavy);
                eldra_eyes_update(g_eyes_ctx, dt_ms);
                compose_frame(g_display_runtime.framebuffer, g_display_runtime.fb_width, g_display_runtime.fb_height, now_ms);
                int eye_dx = 0;
                int eye_dy = 0;
                eldra_eyes_activity_t eye_activity = {0};
                eldra_eyes_get_last_render_center_offset(&eye_dx, &eye_dy);
                eldra_eyes_get_activity(g_eyes_ctx, &eye_activity);
                if (eye_dx != last_eye_dx || eye_dy != last_eye_dy) {
                    EL_LOGI(TAG, "Render center changed eye=(%d,%d)", eye_dx, eye_dy);
                    last_eye_dx = eye_dx;
                    last_eye_dy = eye_dy;
                }

                if (post_reset_probe_stage > 0 && now_ms >= post_reset_probe_next_ms) {
                    int pminx = -1, pmaxx = -1, pminy = -1, pmaxy = -1;
                    bool has_bounds = measure_frame_bounds(g_display_runtime.framebuffer,
                                                          g_display_runtime.fb_width,
                                                          g_display_runtime.fb_height,
                                                          &pminx, &pmaxx, &pminy, &pmaxy);
                    EL_LOGI(TAG,
                            "Post-reset probe stage=%u visible=%d bbox=[%d..%d,%d..%d] eye=(%d,%d)",
                            (unsigned)post_reset_probe_stage,
                            has_bounds ? 1 : 0,
                            pminx, pmaxx, pminy, pmaxy,
                            eye_dx, eye_dy);
                    if (post_reset_probe_stage == 1) {
                        post_reset_probe_stage = 2;
                        post_reset_probe_next_ms = now_ms + 1250U;
                    } else if (post_reset_probe_stage == 2) {
                        post_reset_probe_stage = 3;
                        post_reset_probe_next_ms = now_ms + 1500U;
                    } else if (post_reset_probe_stage == 3) {
                        post_reset_probe_stage = 4;
                        post_reset_probe_next_ms = now_ms + 2000U;
                    } else {
                        post_reset_probe_stage = 0;
                        post_reset_probe_next_ms = 0;
                    }
                }

                if ((now_ms - last_probe_ms) >= k_display_probe_interval_ms) {
                    int minx = 0, maxx = 0, miny = 0, maxy = 0;
                    if (measure_frame_bounds(g_display_runtime.framebuffer,
                                             g_display_runtime.fb_width,
                                             g_display_runtime.fb_height,
                                             &minx, &maxx, &miny, &maxy)) {
                        int cx = (minx + maxx) / 2;
                        int cy = (miny + maxy) / 2;
                        if (last_probe_cx != 0x7FFFFFFF && last_probe_cy != 0x7FFFFFFF) {
                            int jx = cx - last_probe_cx;
                            int jy = cy - last_probe_cy;
                            if (abs(jx) >= k_display_probe_jump_px) {
                                eldra_display_round_diag_t d = {0};
                                eldra_display_round_get_diag(&d);
                                EL_LOGW(TAG,
                                        "Render bbox jump dx=%d dy=%d bbox=[%d..%d,%d..%d] eye_eff=(%d,%d) panel ready=%d sleep=%d reinit=%d sync=%u reset=%u blit_ok=%u fail=%u timeout=%u last_err=%s",
                                        jx, jy,
                                        minx, maxx, miny, maxy,
                                        eye_dx, eye_dy,
                                        d.panel_ready ? 1 : 0,
                                        d.panel_sleeping ? 1 : 0,
                                        d.reinit_in_progress ? 1 : 0,
                                        d.sync_count, d.reset_count,
                                        d.blit_ok_count, d.blit_fail_count, d.blit_timeout_count,
                                        esp_err_to_name(d.last_blit_err));
                            } else if (abs(jx) >= 8 &&
                                       abs(jy) <= 6 &&
                                       !eye_activity.blink_active &&
                                       !eye_activity.look_active &&
                                       !eye_activity.dizzy_active &&
                                       !eye_activity.idle_clip_active &&
                                       (now_ms - last_drift_warn_ms) >= 5000U) {
                                // Track subtle horizontal drift events that happen
                                // while eyes are not intentionally animating.
                                EL_LOGW(TAG, "Unexpected horizontal drift dx=%d dy=%d bbox=[%d..%d,%d..%d] eye_eff=(%d,%d)",
                                        jx, jy, minx, maxx, miny, maxy, eye_dx, eye_dy);
                                last_drift_warn_ms = now_ms;
                            }
                        }
                        last_probe_cx = cx;
                        last_probe_cy = cy;
                    }
                    last_probe_ms = now_ms;
                }
                eyes_lock_give();
                eyes_lock_miss_streak = 0;
            } else {
                eyes_lock_miss_streak++;
                if (eyes_lock_miss_streak == 120) {
                    EL_LOGW(TAG, "Display task starved on eyes lock for ~2s");
                }
            }
        }

        bool panel_ready = eldra_display_round_is_ready();
        if (panel_ready && !g_panel_ready_seen) {
            g_panel_ready_seen = true;
            g_force_render_now = true;
        } else if (!panel_ready) {
            g_panel_ready_seen = false;
        }

        eldra_display_round_diag_t pd = {0};
        eldra_display_round_get_diag(&pd);
        if (pd.reset_count != last_seen_reset_count ||
            pd.panel_sleeping != last_panel_sleeping ||
            pd.reinit_in_progress != last_panel_reinit) {
            bool reset_changed = (pd.reset_count != last_seen_reset_count);
            last_seen_reset_count = pd.reset_count;
            last_panel_sleeping = pd.panel_sleeping;
            last_panel_reinit = pd.reinit_in_progress;
            // Clear stale stall timing whenever panel state transitions. Without
            // this, a long sleep interval can make the next wake look like a
            // multi-minute blit stall to this task's local watchdog.
            last_blit_ok_ms = now_ms;
            last_blit_recover_ms = 0;
            panel_not_ready_since_ms = 0;
            if (reset_changed) {
                post_reset_probe_stage = 1;
                post_reset_probe_next_ms = now_ms + 250U;
            }
        }
        if (!panel_ready) {
            if (!pd.panel_sleeping && !pd.reinit_in_progress) {
                if (panel_not_ready_since_ms == 0) {
                    panel_not_ready_since_ms = now_ms;
                }
                if ((now_ms - panel_not_ready_since_ms) >= 2000U &&
                    (last_auto_reset_ms == 0 || (now_ms - last_auto_reset_ms) >= 5000U)) {
                    EL_LOGW(TAG, "Panel non-ready for %ums (sleep=%d reinit=%d), auto-resetting",
                            (unsigned)(now_ms - panel_not_ready_since_ms),
                            pd.panel_sleeping ? 1 : 0,
                            pd.reinit_in_progress ? 1 : 0);
                    esp_err_t rr = eldra_display_round_reset_panel();
                    last_auto_reset_ms = now_ms;
                    panel_not_ready_since_ms = 0;
                    if (rr == ESP_OK) {
                        g_force_render_now = true;
                    } else {
                        EL_LOGW(TAG, "Auto-reset failed: %s", esp_err_to_name(rr));
                    }
                }
            } else {
                panel_not_ready_since_ms = 0;
            }
        } else {
            panel_not_ready_since_ms = 0;
        }

        if (panel_ready && g_display_runtime.framebuffer) {
            esp_err_t blit_ret = eldra_display_round_blit(g_display_runtime.framebuffer,
                                                          g_display_runtime.fb_width,
                                                          g_display_runtime.fb_height);
            if (blit_ret != ESP_OK) {
                if (now_ms - last_fail_log_ms > 2000U) {
                    EL_LOGW(TAG, "Display blit failed: %s", esp_err_to_name(blit_ret));
                    last_fail_log_ms = now_ms;
                }
                if (!pd.panel_sleeping &&
                    !pd.reinit_in_progress &&
                    (now_ms - last_blit_ok_ms) >= 2000U &&
                    (last_blit_recover_ms == 0U || (now_ms - last_blit_recover_ms) >= 5000U)) {
                    EL_LOGW(TAG, "Display blit stalled for %ums; forcing panel reset",
                            (unsigned)(now_ms - last_blit_ok_ms));
                    esp_err_t rr = eldra_display_round_reset_panel();
                    last_blit_recover_ms = now_ms;
                    if (rr == ESP_OK) {
                        g_force_render_now = true;
                    } else {
                        EL_LOGW(TAG, "Blit-stall reset failed: %s", esp_err_to_name(rr));
                    }
                }
            } else {
                g_display_blit_ok_count++;
                last_blit_ok_ms = now_ms;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (g_force_render_now) {
            g_force_render_now = false;
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(16)); // ~60Hz
        }
    }
}

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
        case EMOTION_STATE_ANGRY: return "ANGRY";
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
        case EMOTION_STATE_ANGRY: return ELDRA_EYES_MOOD_ANGRY;
        case EMOTION_STATE_LONELY: return ELDRA_EYES_MOOD_BORED;
        case EMOTION_STATE_SLEEPY: return ELDRA_EYES_MOOD_SLEEPY;
        case EMOTION_STATE_HUNGRY: return ELDRA_EYES_MOOD_HUNGRY;
        case EMOTION_STATE_ELDRITCH: return ELDRA_EYES_MOOD_ELDRITCH_RUNE;
        case EMOTION_STATE_SCARED: return ELDRA_EYES_MOOD_ANGRY;
        default: return ELDRA_EYES_MOOD_NEUTRAL;
    }
}

static uint32_t eye_modifiers_for_needs(emotion_need_mask_t needs)
{
    uint32_t mods = ELDRA_EYES_MOD_NONE;
    if (needs & EMO_NEED_SLEEPY) mods |= ELDRA_EYES_MOD_SLEEPY;
    if (needs & EMO_NEED_HUNGRY) mods |= ELDRA_EYES_MOD_HUNGRY;
    if (needs & EMO_NEED_LONELY) mods |= ELDRA_EYES_MOD_LONELY;
    if (needs & EMO_NEED_SCARED) mods |= ELDRA_EYES_MOD_SCARED;
    if (needs & EMO_NEED_PLAYFUL) mods |= ELDRA_EYES_MOD_PLAYFUL;
    return mods;
}

static void imu_callback(float gx_dps, float gy_dps, float gz_dps,
                         float ax_g, float ay_g, float az_g, uint32_t dt_ms) {
    bool consumed_by_sleep = eldra_sleep_on_motion(ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps);
    if (consumed_by_sleep) {
        return;
    }
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (eldra_sleep_should_suppress_imu(now_ms)) {
        return;
    }
    bool handled_immediately = false;
    if (g_eyes_ctx && eyes_lock_take(0)) {
        eldra_eyes_handle_imu(g_eyes_ctx, gx_dps, gy_dps, gz_dps, ax_g, ay_g, az_g, dt_ms);
        eyes_lock_give();
        handled_immediately = true;
    }
    if (!handled_immediately) {
        imu_pending_store(gx_dps, gy_dps, gz_dps, ax_g, ay_g, az_g, dt_ms);
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
            EL_LOGI(TAG, "%s", logbuf);
        } else {
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "Auto WiFi attempt %d failed to start: %s", attempt, esp_err_to_name(w));
            EL_LOGW(TAG, "%s", logbuf);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
        wifi_driver_get_status(&st, &ap);
        EL_LOGI(TAG, "Auto WiFi attempt %d status=%d RSSI=%d", attempt, st, ap.rssi);
        if (st == WIFI_STATUS_CONNECTED) {
            EL_LOGI(TAG, "WiFi connected to \"%s\" RSSI=%d", ap.ssid, ap.rssi);
            vTaskDelete(NULL);
            return;
        }
        if (attempt < max_attempts) {
            vTaskDelay(attempt_delay);
        }
    }
    EL_LOGW(TAG, "Auto WiFi retries exhausted");
    vTaskDelete(NULL);
}

static void on_ip_acquired(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    (void)data;
    g_has_ip = true;
    g_wifi_online = true;
    g_net_state_dirty = true;
    g_force_render_now = true;
}

static void on_wifi_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    g_has_ip = false;
    g_wifi_online = false;
    g_net_state_dirty = true;
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
    if (cfg->mood_state >= 0 && cfg->mood_state <= EMOTION_STATE_ANGRY) {
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

static void rtc_restore_task(void *arg)
{
    (void)arg;
    esp_err_t rtc_restore = rtc_driver_restore_persisted();
    if (rtc_restore == ESP_OK) {
        EL_LOGI(TAG, "System time restored from persisted RTC snapshot");
    } else {
        // Informational only: missing/invalid persisted time is expected on
        // first boot or after a storage reset.
        EL_LOGI(TAG, "RTC restore skipped: %s", esp_err_to_name(rtc_restore));
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    log_init();
    log_set_console_level(LOG_LEVEL_INFO);
    esp_log_level_set("*", ESP_LOG_INFO);
    EL_LOGI(TAG, "app_main start");
    esp_reset_reason_t rr = esp_reset_reason();
    EL_LOGI(TAG, "Reset reason: %s (%d)", reset_reason_name(rr), (int)rr);

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
    g_sd_io_guard_ready = true;
    eldra_display_round_set_backlight(90);

    EL_LOGI(TAG, "Boot stage: netif/event init");
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
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    EL_LOGI(TAG, "Boot stage: netif/event init done");

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
    EL_LOGI(TAG, "Boot stage: console modules ready");

    EL_LOGI(TAG, "Boot stage: load config defaults");
    config_store_t cfg = {0};
    config_store_get_defaults(&cfg);

    EL_LOGI(TAG, "Boot stage: NVS fallback removed (SD/default only)");

    bool cfg_loaded = false;
    EL_LOGI(TAG, "Boot stage: sd_driver_init begin");
    if (sd_driver_init() == ESP_OK) {
        EL_LOGI(TAG, "Boot stage: sd_driver_init done");
        log_sd_notify_mounted();
        EL_LOGI(TAG, "Boot stage: config_store_load begin");
        if (config_store_load(&cfg) == ESP_OK) {
            cfg_loaded = true;
        } else {
            EL_LOGW(TAG, "Config load failed");
        }
    } else {
        EL_LOGW(TAG, "SD init failed; skipping config load");
    }
    EL_LOGI(TAG, "Boot stage: sd/config complete loaded=%d", cfg_loaded ? 1 : 0);

    // NVS fallback disabled by request; keep defaults when SD/config is unavailable.

    // Apply configurable sleep window and mood log interval to the emotion engine.
    emotion_set_sleep_window((uint8_t)cfg.sleep_start_hour, (uint8_t)cfg.sleep_end_hour);
    eldra_sleep_set_window((uint8_t)cfg.sleep_start_hour, (uint8_t)cfg.sleep_end_hour);
    eldra_sleep_set_thresholds((uint32_t)cfg.sleep_window_inactivity_ms,
                               (uint8_t)cfg.sleep_low_battery_pct,
                               (uint32_t)cfg.sleep_global_inactivity_ms,
                               (uint32_t)cfg.sleep_overfed_hold_ms);
    emotion_set_mood_log_interval_minutes((uint32_t)cfg.mood_log_interval_minutes);
    EL_LOGI(TAG, "Mood log interval source=%s value=%d min",
            cfg_loaded ? "config" : "defaults/fallback",
            cfg.mood_log_interval_minutes);
    emotion_set_angry_policy((uint8_t)cfg.angry_dizzy_count_threshold,
                             (uint32_t)cfg.angry_dizzy_window_ms,
                             (uint32_t)cfg.angry_override_min_ms,
                             (uint32_t)cfg.angry_override_max_ms);
    apply_saved_mood(&cfg);
    int raw_glyph_x = cfg.glyph_offset_x;
    int raw_glyph_y = cfg.glyph_offset_y;
    int raw_sleep_lid_depth = cfg.sleep_lid_depth;
    int raw_angry_lid_depth = cfg.angry_lid_depth;
    cfg.glyph_offset_x = clamp_int(cfg.glyph_offset_x, -400, 400);
    cfg.glyph_offset_y = clamp_int(cfg.glyph_offset_y, -400, 400);
    cfg.sleep_lid_depth = clamp_int(cfg.sleep_lid_depth, 0, 6);
    cfg.angry_lid_depth = clamp_int(cfg.angry_lid_depth, 0, 6);
    if (cfg_loaded && (cfg.glyph_offset_x != raw_glyph_x || cfg.glyph_offset_y != raw_glyph_y)) {
        (void)config_store_save(&cfg);
        EL_LOGW(TAG, "Clamped persisted glyph offsets (%d,%d) -> (%d,%d)",
                raw_glyph_x, raw_glyph_y, cfg.glyph_offset_x, cfg.glyph_offset_y);
    }
    if (cfg_loaded && (cfg.sleep_lid_depth != raw_sleep_lid_depth || cfg.angry_lid_depth != raw_angry_lid_depth)) {
        (void)config_store_save(&cfg);
        EL_LOGW(TAG, "Clamped persisted lid depths sleep=%d->%d angry=%d->%d",
                raw_sleep_lid_depth, cfg.sleep_lid_depth,
                raw_angry_lid_depth, cfg.angry_lid_depth);
    }
    g_last_mood_save_ms = (uint64_t)start_ms;
    g_last_time_persist_ms = (uint64_t)start_ms;
    g_cfg_current = cfg; // stash for later event reapplication
    log_sd_set_enabled(cfg.logs_to_sd);
    if (k_force_runtime_sd_logging_off && cfg.logs_to_sd) {
        log_sd_set_enabled(false);
        EL_LOGW(TAG, "Runtime SD logging forced off for display stability isolation");
    }

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
            esp_err_t wi = wifi_driver_init_sta();
            if (wi != ESP_OK) {
                EL_LOGW(TAG, "Auto WiFi init failed: %s", esp_err_to_name(wi));
            }
            if (cfg.wifi_ssid[0] != '\0') {
                wifi_driver_set_roaming(cfg.wifi_roam);
                strlcpy(s_auto_wifi_cfg.ssid, cfg.wifi_ssid, sizeof(s_auto_wifi_cfg.ssid));
                strlcpy(s_auto_wifi_cfg.pass, cfg.wifi_pass, sizeof(s_auto_wifi_cfg.pass));
                s_auto_wifi_cfg.roam = cfg.wifi_roam;
                if (xTaskCreatePinnedToCore(auto_wifi_task, "auto_wifi", 4096, NULL, 4, NULL, 0) != pdPASS) {
                    EL_LOGE(TAG, "Auto WiFi: failed to create retry task");
                } else {
                    EL_LOGI(TAG, "Auto WiFi retry task started for \"%s\"", cfg.wifi_ssid);
                }
            } else {
                EL_LOGW(TAG, "Auto WiFi enabled but no saved credentials");
            }
        }

        if (cfg.auto_init_imu) {
            esp_err_t ir = qmi8658_init();
            if (ir == ESP_OK) {
                EL_LOGI(TAG, "IMU auto-init OK");
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "IMU auto-init failed: %s", esp_err_to_name(ir));
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
    if (!g_eyes_lock) {
        g_eyes_lock = xSemaphoreCreateMutex();
    }
    if (!g_eyes_lock) {
        EL_LOGE(TAG, "Failed to create eyes lock; holding");
        goto fail_safe;
    }
    // Initialize glyph runtime before applying persisted placement values.
    eldra_glyphs_init(NULL);
    eldra_glyphs_set_layout(GLYPH_LAYOUT_STACK);
    eldra_glyphs_hide_all();

    // Apply persisted display/eye center offsets if available.
    eldra_eyes_set_display_center_offset(cfg.display_center_x_offset, cfg.display_center_y_offset);
    eldra_glyphs_set_display_center_offset(cfg.display_center_x_offset, cfg.display_center_y_offset);
    eldra_eyes_set_center_offset(cfg.eyes_center_x_offset, cfg.eyes_center_y_offset);
    eldra_eyes_set_lid_depths((uint8_t)cfg.sleep_lid_depth, (uint8_t)cfg.angry_lid_depth);
    eldra_glyphs_set_offset(0, cfg.glyph_offset_x, cfg.glyph_offset_y);
    eldra_glyphs_set_scale(cfg.glyph_scale);
    int eff_eye_x = 0, eff_eye_y = 0;
    eldra_eyes_get_effective_center_offset(&eff_eye_x, &eff_eye_y);
    EL_LOGI(TAG, "Applied offsets: disp=(%d,%d) eye_req=(%d,%d) eye_eff=(%d,%d) glyph=(%d,%d) scale=%d lids=(sleep=%d angry=%d)",
            cfg.display_center_x_offset, cfg.display_center_y_offset,
            cfg.eyes_center_x_offset, cfg.eyes_center_y_offset,
            eff_eye_x, eff_eye_y,
            cfg.glyph_offset_x, cfg.glyph_offset_y, cfg.glyph_scale,
            cfg.sleep_lid_depth, cfg.angry_lid_depth);
    g_eyes_ctx = eyes_ctx;
    eldra_sensors_set_imu_callback(imu_callback);

    EL_LOGI(TAG, "Boot stage: framebuffer alloc begin (%dx%d)", fb_width, fb_height);
    size_t buf_size_bytes = (size_t)fb_width * (size_t)fb_height * sizeof(uint16_t);
    uint16_t *framebuffer = (uint16_t *)heap_caps_malloc(buf_size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!framebuffer) {
        framebuffer = (uint16_t *)heap_caps_malloc(buf_size_bytes, MALLOC_CAP_8BIT);
        if (!framebuffer) {
            EL_LOGE(TAG, "Framebuffer alloc failed; holding");
            goto fail_safe;
        }
    }
    EL_LOGI(TAG, "Boot stage: framebuffer alloc done");
    memset(framebuffer, 0, buf_size_bytes);
    EL_LOGI(TAG, "Boot stage: boot test blit skipped (non-blocking startup)");

    if (k_auto_calibrate_on_boot) {
        // Auto-calibrate eye centering once at boot using current offsets and persist if config is loaded.
        if (eyes_lock_take(pdMS_TO_TICKS(20))) {
            auto_calibrate_eyes(eyes_ctx, framebuffer, fb_width, fb_height, cfg_loaded ? &cfg : NULL, cfg_loaded);
            eyes_lock_give();
        }
    } else {
        EL_LOGI(TAG, "Auto-calibrate on boot disabled; use disp_center/eyes_offset then persist.");
    }

    EL_LOGI(TAG, "Boot stage: sleep init begin");
    eldra_sleep_init(&g_emotion);
    EL_LOGI(TAG, "Boot stage: sleep init done");
    g_display_runtime.framebuffer = framebuffer;
    g_display_runtime.fb_width = fb_width;
    g_display_runtime.fb_height = fb_height;
    g_force_render_now = true;
    g_panel_ready_seen = false;
    EL_LOGI(TAG, "Boot stage: display task create begin");
    if (xTaskCreatePinnedToCore(display_task, "display_task", 6144, NULL, 5, &g_display_task_handle, 1) != pdPASS) {
        EL_LOGE(TAG, "Failed to create display task; holding");
        goto fail_safe;
    }
    EL_LOGI(TAG, "Display task started");
    EL_LOGW(TAG, "App-level watchdog/monitor disabled by request");

    // Restore persisted RTC time asynchronously so boot cannot block on I2C/NVS.
    if (xTaskCreate(rtc_restore_task, "rtc_restore", 3072, NULL, 2, NULL) != pdPASS) {
        EL_LOGW(TAG, "RTC restore task create failed");
    }

    uint64_t last_us = esp_timer_get_time();
    uint32_t last_heartbeat_ms = (uint32_t)(last_us / 1000ULL);
    uint32_t blit_watch_last_ok = g_display_blit_ok_count;
    uint32_t blit_watch_since_ms = last_heartbeat_ms;
    uint32_t blit_watch_last_recover_ms = 0;
    while (1) {
        uint64_t now_us = esp_timer_get_time();
        last_us = now_us;

        uint32_t now_ms = (uint32_t)(now_us / 1000ULL);

        if (g_net_state_dirty) {
            bool online = g_has_ip;
            g_net_state_dirty = false;

            if (online) {
                esp_netif_ip_info_t ip = {0};
                esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
                    EL_LOGI(TAG, "Got IP: " IPSTR ", mask " IPSTR ", gw " IPSTR,
                            IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw));
                } else {
                    EL_LOGI(TAG, "Network online");
                }

                if (!sntp_started) {
                    sntp_started = true;
                    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
                    esp_netif_sntp_init(&sntp_cfg);
                    EL_LOGI(TAG, "SNTP started for timezone EST5EDT");
                }
            } else {
                EL_LOGW(TAG, "WiFi disconnected");
            }

            if (g_cloud_ready) {
                eldra_cloud_set_online(online);
            }
        }

        if (g_emotion_ready) {
            emotion_set_battery_percent(&g_emotion, eldra_sensors_get_battery_percent());
            emotion_on_tick(&g_emotion, now_ms);
            if (g_emotion.last_mood_log_ms != 0 && g_emotion.last_mood_log_ms != g_last_seen_mood_log_ms) {
                g_last_seen_mood_log_ms = g_emotion.last_mood_log_ms;
                uint32_t mood_delta_ms = (g_prev_mood_log_ms == 0) ? 0 : (g_emotion.last_mood_log_ms - g_prev_mood_log_ms);
                g_prev_mood_log_ms = g_emotion.last_mood_log_ms;
                uint32_t mood_interval_ms = emotion_get_mood_log_interval_ms();
                int req_eye_x = 0, req_eye_y = 0;
                int eff_eye_x = 0, eff_eye_y = 0;
                int render_eye_x = 0, render_eye_y = 0;
                int disp_x = 0, disp_y = 0;
                eldra_eyes_get_center_offset(&req_eye_x, &req_eye_y);
                eldra_eyes_get_effective_center_offset(&eff_eye_x, &eff_eye_y);
                eldra_eyes_get_last_render_center_offset(&render_eye_x, &render_eye_y);
                eldra_eyes_get_display_center_offset(&disp_x, &disp_y);
                int minx = -1, maxx = -1, miny = -1, maxy = -1;
                int bbox_cx = -1, bbox_cy = -1;
                uint32_t blit_ok_task_now = g_display_blit_ok_count;
                uint32_t blit_ok_task_delta = blit_ok_task_now - g_prev_display_blit_ok_count;
                g_prev_display_blit_ok_count = blit_ok_task_now;
                if (g_display_runtime.framebuffer && eyes_lock_take(pdMS_TO_TICKS(2))) {
                    if (measure_frame_bounds(g_display_runtime.framebuffer,
                                             g_display_runtime.fb_width,
                                             g_display_runtime.fb_height,
                                             &minx, &maxx, &miny, &maxy)) {
                        bbox_cx = (minx + maxx) / 2;
                        bbox_cy = (miny + maxy) / 2;
                    }
                    eyes_lock_give();
                }
                eldra_display_round_diag_t d = {0};
                eldra_display_round_get_diag(&d);
                EL_LOGW(TAG,
                        "Mood-log checkpoint t=%lu interval=%lums delta=%lums state=%s eye_req=(%d,%d) eye_eff=(%d,%d) eye_render=(%d,%d) disp=(%d,%d) bbox=[%d..%d,%d..%d] bbox_c=(%d,%d) panel ready=%d sleep=%d reinit=%d sync=%u reset=%u blit_ok=%u fail=%u timeout=%u task_blit_ok=%lu task_blit_delta=%lu imu_coalesce=%lu imu_max_dt=%lu last_err=%s",
                        (unsigned long)g_emotion.last_mood_log_ms,
                        (unsigned long)mood_interval_ms,
                        (unsigned long)mood_delta_ms,
                        emotion_state_str(g_emotion.current_state),
                        req_eye_x, req_eye_y,
                        eff_eye_x, eff_eye_y,
                        render_eye_x, render_eye_y,
                        disp_x, disp_y,
                        minx, maxx, miny, maxy, bbox_cx, bbox_cy,
                        d.panel_ready ? 1 : 0,
                        d.panel_sleeping ? 1 : 0,
                        d.reinit_in_progress ? 1 : 0,
                        d.sync_count, d.reset_count,
                        d.blit_ok_count, d.blit_fail_count, d.blit_timeout_count,
                        (unsigned long)blit_ok_task_now, (unsigned long)blit_ok_task_delta,
                        (unsigned long)g_imu_coalesce_events,
                        (unsigned long)g_imu_coalesce_max_dt_ms,
                        esp_err_to_name(d.last_blit_err));
            }
            comms_process_all_pending(&g_emotion, now_ms);
            eldra_eyes_mood_t desired = mood_for_state(g_emotion.current_state);
            uint32_t desired_mods = eye_modifiers_for_needs(emotion_get_needs(&g_emotion));
            if (eyes_lock_take(pdMS_TO_TICKS(2))) {
                if (eldra_eyes_get_mood(eyes_ctx) != desired) {
                    eldra_eyes_set_mood(eyes_ctx, desired);
                }
                if (eldra_eyes_get_modifiers(eyes_ctx) != desired_mods) {
                    eldra_eyes_set_modifiers(eyes_ctx, desired_mods);
                }
                eyes_lock_give();
            }
        }

        push_cloud_state_if_ready(now_ms);

        // Heartbeat every few seconds to SD/console to catch silent hangs.
        if (k_enable_heartbeat && (now_ms - last_heartbeat_ms) >= k_heartbeat_interval_ms) {
            last_heartbeat_ms = now_ms;
            char hb[64];
            snprintf(hb, sizeof(hb), "HB t=%lu wifi=%d cloud=%d", (unsigned long)now_ms, g_wifi_online, g_cloud_ready);
            EL_LOGI(TAG, "%s", hb);
        }

        if (k_mood_save_interval_ms > 0 &&
            (now_ms - (uint32_t)g_last_mood_save_ms) >= k_mood_save_interval_ms) {
            save_mood_to_config();
            g_last_mood_save_ms = now_ms;
        }
        if (k_time_persist_interval_ms > 0 &&
            (now_ms - (uint32_t)g_last_time_persist_ms) >= k_time_persist_interval_ms) {
            (void)rtc_driver_persist_if_valid();
            g_last_time_persist_ms = now_ms;
        }

        // Main-loop safety guard: if display blits stop advancing while panel is
        // otherwise marked ready, perform one controlled panel reset.
        eldra_display_round_diag_t dguard = {0};
        eldra_display_round_get_diag(&dguard);
        uint32_t blit_now = g_display_blit_ok_count;
        bool sleep_active_guard = eldra_sleep_is_active();
        if (sleep_active_guard || !dguard.panel_ready || dguard.panel_sleeping || dguard.reinit_in_progress) {
            blit_watch_last_ok = blit_now;
            blit_watch_since_ms = now_ms;
        } else if (blit_now != blit_watch_last_ok) {
            blit_watch_last_ok = blit_now;
            blit_watch_since_ms = now_ms;
        } else if (dguard.panel_ready &&
                   !dguard.panel_sleeping &&
                   !dguard.reinit_in_progress &&
                   (now_ms - blit_watch_since_ms) >= 3000U &&
                   (blit_watch_last_recover_ms == 0U || (now_ms - blit_watch_last_recover_ms) >= 5000U)) {
            EL_LOGW(TAG, "Display blit counter stalled for %ums (ok=%lu); forcing panel reset",
                    (unsigned)(now_ms - blit_watch_since_ms),
                    (unsigned long)blit_now);
            esp_err_t rr_guard = eldra_display_round_reset_panel();
            blit_watch_last_recover_ms = now_ms;
            blit_watch_since_ms = now_ms;
            if (rr_guard == ESP_OK) {
                g_force_render_now = true;
            } else {
                EL_LOGW(TAG, "Display stall reset failed: %s", esp_err_to_name(rr_guard));
            }
        }

        eldra_sleep_tick(now_ms);

        // Sleepy eyes: use a lighter lid generally, then heavier droop only in
        // the final 60 seconds before sleep shutdown stage.
        uint32_t sleep_eta_ms = 0;
        bool sleep_countdown_active = eldra_sleep_get_shutdown_eta_ms(now_ms, &sleep_eta_ms);
        g_sleep_lid_heavy = sleep_countdown_active && (sleep_eta_ms <= 60000U);

        vTaskDelay(pdMS_TO_TICKS(16)); // ~60 FPS pacing
    }

fail_safe:
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// External hook used by console commands to force the next render
void eldra_request_render_now(void)
{
    g_force_render_now = true;
}
