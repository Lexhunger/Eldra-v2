#include "eldra_sleep.h"
#include "eldra_glyphs.h"
#include "eldra_emotion.h"
#include "eldra_display_round.h"
#include "eldra_cloud.h"
#include "wifi_driver.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <math.h>
#include <time.h>
#include "eldra_logging.h"

static const char *TAG = "eldra_sleep";
void eldra_request_render_now(void) __attribute__((weak));

typedef enum {
    SLEEP_MSG_VISUAL = 1,
    SLEEP_MSG_SHUTDOWN,
    SLEEP_MSG_WAKE,
} sleep_msg_t;

static bool s_sleeping = false;
static bool s_power_stage_done = false;
static bool s_panel_sleep_applied = false;
static bool s_panel_shutdown_attempted = false;
static uint64_t s_shutdown_time_ms = 0;
static uint64_t s_last_activity_ms = 0;
static uint64_t s_sleep_started_ms = 0;
static uint64_t s_last_sleep_transition_ms = 0;
static uint64_t s_overfed_sleepy_since_ms = 0;
static uint64_t s_motion_qualify_since_ms = 0;
static uint32_t s_last_seen_interaction_ms = 0;
static bool s_sleep_reason_window = false;
static bool s_glyph_inited = false;
static emotion_context_t *s_emotion = NULL;
static uint8_t s_sleep_start_hour = 22;
static uint8_t s_sleep_end_hour = 8;
static QueueHandle_t s_msg_q = NULL;
static SemaphoreHandle_t s_state_lock = NULL;

static const uint32_t k_shutdown_min_ms = 120000;          // 2 minutes
static const uint32_t k_shutdown_jitter_ms = 180000;       // +0..3 minutes
static const uint8_t k_backlight_awake = 90;
static const uint8_t k_backlight_sleep_visual = 30;
// Temporary stability mode: keep the panel streaming and only gate backlight
// during sleep. Full panel sleep/reset wake path remains available but has
// shown non-deterministic black-screen behavior on this board.
static const bool k_enable_panel_power_stage = false;

static uint32_t s_window_inactivity_ms = 300000;     // 5 minutes
static uint32_t s_global_inactivity_ms = 1800000;    // 30 minutes
static uint32_t s_overfed_hold_ms = 120000;          // 2 minutes
static uint8_t s_low_battery_pct = 10;               // 10%
static const time_t k_valid_time_epoch_min = 1704067200; // 2024-01-01T00:00:00Z
static const uint32_t k_wake_motion_guard_ms = 1500;       // ignore same-motion wake burst right after entering sleep
static const uint32_t k_post_wake_inactivity_grace_ms = 10000; // avoid immediate re-sleep loops after wake
static const uint32_t k_post_wake_imu_guard_ms = 2500;     // ignore wake shake for reactive eye IMU effects
static const float k_activity_gdev_thresh = 0.06f;
static const float k_activity_gyro_thresh_dps = 12.0f;
static const float k_activity_strong_gdev_thresh = 0.12f;
static const float k_activity_strong_gyro_thresh_dps = 30.0f;
static const uint32_t k_activity_qualify_ms = 200;
static const float k_wake_gdev_thresh = 0.12f;
static const float k_wake_gyro_thresh_dps = 45.0f;

static inline uint64_t elapsed_ms_u64(uint64_t start_ms, uint64_t now_ms)
{
    return (now_ms >= start_ms) ? (now_ms - start_ms) : 0;
}

static bool get_local_hour_if_valid(int *hour_out)
{
    time_t t = time(NULL);
    if (t < k_valid_time_epoch_min) {
        return false;
    }

    struct tm tm_now;
    localtime_r(&t, &tm_now);
    if (hour_out) {
        *hour_out = tm_now.tm_hour;
    }
    return true;
}

static bool is_in_sleep_window(int hour)
{
    if (s_sleep_start_hour == s_sleep_end_hour) {
        return true;
    }
    if (s_sleep_start_hour < s_sleep_end_hour) {
        return hour >= s_sleep_start_hour && hour < s_sleep_end_hour;
    }
    return (hour >= s_sleep_start_hour) || (hour < s_sleep_end_hour);
}

static void post_sleep_msg(sleep_msg_t msg)
{
    if (!s_msg_q) {
        return;
    }
    if (xQueueSend(s_msg_q, &msg, 0) != pdTRUE) {
        EL_LOGW(TAG, "Sleep queue full; dropping msg=%d", (int)msg);
    }
}

static void drain_sleep_queue(void)
{
    if (!s_msg_q) {
        return;
    }
    sleep_msg_t dropped;
    while (xQueueReceive(s_msg_q, &dropped, 0) == pdTRUE) {
        // Drop stale stage messages on state transitions.
    }
}

static bool snapshot_sleeping(void)
{
    bool sleeping = s_sleeping;
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        sleeping = s_sleeping;
        xSemaphoreGive(s_state_lock);
    }
    return sleeping;
}

static bool snapshot_panel_sleep_applied(void)
{
    bool applied = s_panel_sleep_applied;
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        applied = s_panel_sleep_applied;
        xSemaphoreGive(s_state_lock);
    }
    return applied;
}

static bool snapshot_panel_shutdown_attempted(void)
{
    bool attempted = s_panel_shutdown_attempted;
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        attempted = s_panel_shutdown_attempted;
        xSemaphoreGive(s_state_lock);
    }
    return attempted;
}

static bool snapshot_sleep_reason_window(void)
{
    bool window_reason = s_sleep_reason_window;
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        window_reason = s_sleep_reason_window;
        xSemaphoreGive(s_state_lock);
    }
    return window_reason;
}

static void sleep_now_internal(bool reason_window);

static void sleep_worker_task(void *arg)
{
    (void)arg;
    sleep_msg_t msg;
    while (xQueueReceive(s_msg_q, &msg, portMAX_DELAY) == pdTRUE) {
        switch (msg) {
            case SLEEP_MSG_VISUAL:
                if (!snapshot_sleeping()) {
                    EL_LOGI(TAG, "Skip stale visual stage (already awake)");
                    break;
                }
                EL_LOGI(TAG, "Sleep stage: visual");
                eldra_display_round_set_backlight(k_backlight_sleep_visual);
                break;

            case SLEEP_MSG_SHUTDOWN: {
                if (!snapshot_sleeping()) {
                    EL_LOGI(TAG, "Skip stale shutdown stage (already awake)");
                    break;
                }
                EL_LOGW(TAG, "Sleep stage: shutdown (cloud/wifi/panel)");
                eldra_cloud_set_online(false);

                esp_err_t w = esp_wifi_stop();
                if (w != ESP_OK && w != ESP_ERR_INVALID_STATE && w != ESP_ERR_WIFI_NOT_INIT) {
                    EL_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(w));
                }

                eldra_display_round_set_backlight(0);
                if (!k_enable_panel_power_stage) {
                    if (s_state_lock) {
                        xSemaphoreTake(s_state_lock, portMAX_DELAY);
                        s_panel_shutdown_attempted = false;
                        s_panel_sleep_applied = false;
                        xSemaphoreGive(s_state_lock);
                    }
                    EL_LOGI(TAG, "Panel shutdown disabled; visual sleep only");
                    break;
                }
                if (s_state_lock) {
                    xSemaphoreTake(s_state_lock, portMAX_DELAY);
                    s_panel_shutdown_attempted = true;
                    xSemaphoreGive(s_state_lock);
                }
                esp_err_t d = ESP_ERR_TIMEOUT;
                for (int attempt = 1; attempt <= 5; ++attempt) {
                    d = eldra_display_round_panel_sleep(true);
                    if (d == ESP_OK || d == ESP_ERR_INVALID_STATE) {
                        break;
                    }
                    if (d == ESP_ERR_TIMEOUT) {
                        vTaskDelay(pdMS_TO_TICKS(25));
                        continue;
                    }
                    break;
                }
                EL_LOGI(TAG, "Panel sleep: %s", esp_err_to_name(d));
                if (d == ESP_OK && s_state_lock) {
                    xSemaphoreTake(s_state_lock, portMAX_DELAY);
                    s_panel_sleep_applied = true;
                    xSemaphoreGive(s_state_lock);
                }
                break;
            }

            case SLEEP_MSG_WAKE: {
                if (snapshot_sleeping()) {
                    EL_LOGI(TAG, "Skip stale wake stage (sleep re-entered)");
                    break;
                }
                EL_LOGI(TAG, "Sleep stage: wake");
                // If shutdown was even attempted, force a full panel reset.
                // The panel can be left in a blank/bad state even when
                // panel_sleep(true) timed out and never marked sleeping=true.
                bool need_panel_reset = snapshot_panel_sleep_applied() ||
                                        snapshot_panel_shutdown_attempted() ||
                                        !eldra_display_round_is_ready();
                esp_err_t d = ESP_FAIL;
                for (int attempt = 1; attempt <= 3; ++attempt) {
                    d = need_panel_reset
                        ? eldra_display_round_reset_panel()
                        : eldra_display_round_panel_sleep(false);
                    if (d == ESP_OK || d == ESP_ERR_INVALID_STATE) {
                        break;
                    }
                    EL_LOGW(TAG, "Panel wake retry %d failed: %s", attempt, esp_err_to_name(d));
                    vTaskDelay(pdMS_TO_TICKS(150));
                }
                if (d != ESP_OK && d != ESP_ERR_INVALID_STATE) {
                    EL_LOGW(TAG, "Panel wake failed after retries: %s", esp_err_to_name(d));
                } else if (need_panel_reset) {
                    EL_LOGI(TAG, "Panel wake reset complete");
                } else {
                    EL_LOGI(TAG, "Panel wake reconciled without reset");
                }
                if (s_state_lock) {
                    xSemaphoreTake(s_state_lock, portMAX_DELAY);
                    s_panel_sleep_applied = false;
                    s_panel_shutdown_attempted = false;
                    xSemaphoreGive(s_state_lock);
                }

                eldra_display_round_set_backlight(k_backlight_awake);
                if (eldra_request_render_now) {
                    eldra_request_render_now();
                }

                esp_err_t w = esp_wifi_start();
                if (w == ESP_ERR_WIFI_NOT_INIT) {
                    EL_LOGI(TAG, "WiFi not initialized; skip wifi_start on wake");
                } else if (w != ESP_OK && w != ESP_ERR_INVALID_STATE) {
                    EL_LOGW(TAG, "esp_wifi_start failed: %s", esp_err_to_name(w));
                }

                wifi_driver_status_t st = WIFI_STATUS_IDLE;
                (void)wifi_driver_get_status(&st, NULL);
                eldra_cloud_set_online(st == WIFI_STATUS_CONNECTED);
                break;
            }

            default:
                break;
        }
    }

    vTaskDelete(NULL);
}

void eldra_sleep_init(emotion_context_t *ctx)
{
    s_emotion = ctx;
    if (!eldra_glyphs_is_initialized()) {
        eldra_glyphs_init(NULL);
        eldra_glyphs_set_layout(GLYPH_LAYOUT_STACK);
        eldra_glyphs_hide_all();
    }
    s_glyph_inited = true;

    s_state_lock = xSemaphoreCreateMutex();
    s_msg_q = xQueueCreate(8, sizeof(sleep_msg_t));
    if (!s_state_lock || !s_msg_q) {
        EL_LOGE(TAG, "sleep init failed (lock/queue alloc)");
        return;
    }

    if (xTaskCreate(sleep_worker_task, "sleep_worker", 4096, NULL, 3, NULL) != pdPASS) {
        EL_LOGE(TAG, "sleep worker task create failed");
        return;
    }

    s_last_activity_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_sleep_started_ms = 0;
    s_last_sleep_transition_ms = s_last_activity_ms;
    s_overfed_sleepy_since_ms = 0;
    s_motion_qualify_since_ms = 0;
    s_last_seen_interaction_ms = (ctx != NULL) ? ctx->last_interaction_ms : 0;
    s_panel_sleep_applied = false;
    s_panel_shutdown_attempted = false;
    EL_LOGI(TAG, "sleep subsystem ready");
}

void eldra_sleep_set_window(uint8_t start_hour, uint8_t end_hour)
{
    if (start_hour < 24) {
        s_sleep_start_hour = start_hour;
    }
    if (end_hour < 24) {
        s_sleep_end_hour = end_hour;
    }
    EL_LOGI(TAG, "Sleep window set start=%u end=%u",
             (unsigned)s_sleep_start_hour,
             (unsigned)s_sleep_end_hour);
}

void eldra_sleep_set_thresholds(uint32_t window_inactivity_ms,
                                uint8_t low_battery_pct,
                                uint32_t global_inactivity_ms,
                                uint32_t overfed_hold_ms)
{
    // Keep sane lower bounds to avoid accidental instant-sleep loops.
    if (window_inactivity_ms < 60000U) window_inactivity_ms = 60000U;
    if (global_inactivity_ms < 60000U) global_inactivity_ms = 60000U;
    if (overfed_hold_ms < 30000U) overfed_hold_ms = 30000U;
    if (low_battery_pct > 100U) low_battery_pct = 100U;

    s_window_inactivity_ms = window_inactivity_ms;
    s_low_battery_pct = low_battery_pct;
    s_global_inactivity_ms = global_inactivity_ms;
    s_overfed_hold_ms = overfed_hold_ms;

    EL_LOGI(TAG, "Sleep thresholds set: window_idle=%ums low_batt=%u%% global_idle=%ums overfed_hold=%ums",
             (unsigned)s_window_inactivity_ms,
             (unsigned)s_low_battery_pct,
             (unsigned)s_global_inactivity_ms,
             (unsigned)s_overfed_hold_ms);
}

void eldra_sleep_sleep_now(void)
{
    sleep_now_internal(false);
}

static void sleep_now_internal(bool reason_window)
{
    uint32_t delay_ms = k_shutdown_min_ms + (esp_random() % k_shutdown_jitter_ms);
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
    if (s_sleeping) {
        if (s_state_lock) {
            xSemaphoreGive(s_state_lock);
        }
        return;
    }

    s_shutdown_time_ms = now_ms + delay_ms;
    s_sleeping = true;
    s_power_stage_done = false;
    s_panel_sleep_applied = false;
    s_panel_shutdown_attempted = false;
    s_sleep_reason_window = reason_window;
    s_sleep_started_ms = now_ms;
    s_last_sleep_transition_ms = now_ms;

    if (s_state_lock) {
        xSemaphoreGive(s_state_lock);
    }

    if (s_glyph_inited) {
        eldra_glyphs_show(GLYPH_SLEEP, 0);
    }
    if (s_emotion) {
        emotion_force_sleep(s_emotion, true, now_ms);
    }
    drain_sleep_queue();
    post_sleep_msg(SLEEP_MSG_VISUAL);

    EL_LOGI(TAG, "Sleep requested; shutdown staging in %u ms", delay_ms);
}

void eldra_sleep_wake_now(void)
{
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
    s_sleeping = false;
    s_power_stage_done = false;
    s_shutdown_time_ms = 0;
    s_last_activity_ms = now_ms;
    s_sleep_started_ms = 0;
    s_last_sleep_transition_ms = now_ms;
    s_overfed_sleepy_since_ms = 0;
    s_motion_qualify_since_ms = 0;
    s_panel_sleep_applied = false;
    s_sleep_reason_window = false;
    s_last_seen_interaction_ms = (s_emotion != NULL) ? s_emotion->last_interaction_ms : s_last_seen_interaction_ms;
    if (s_state_lock) {
        xSemaphoreGive(s_state_lock);
    }

    if (s_glyph_inited) {
        int hour = 0;
        bool within_window = get_local_hour_if_valid(&hour) && is_in_sleep_window(hour);
        if (within_window) {
            eldra_glyphs_show(GLYPH_SLEEP, 0);
        } else {
            eldra_glyphs_hide_unforced();
        }
    }
    if (s_emotion) {
        emotion_force_sleep(s_emotion, false, now_ms);
    }
    // Immediate UX recovery while the worker performs full panel wake/reset.
    eldra_display_round_set_backlight(k_backlight_awake);
    if (eldra_request_render_now) {
        eldra_request_render_now();
    }
    drain_sleep_queue();
    post_sleep_msg(SLEEP_MSG_WAKE);

    EL_LOGI(TAG, "Wake requested; sleep cancelled");
}

void eldra_sleep_set_glyph_offset(int dx, int dy)
{
    if (s_glyph_inited) {
        eldra_glyphs_set_offset(0, dx, dy);
    }
}

void eldra_sleep_tick(uint64_t now_ms)
{
    bool sleeping = false;
    bool power_stage_done = false;
    uint64_t shutdown_at_ms = 0;

    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        sleeping = s_sleeping;
        power_stage_done = s_power_stage_done;
        shutdown_at_ms = s_shutdown_time_ms;
        xSemaphoreGive(s_state_lock);
    }

    if (!sleeping) {
        uint64_t last_activity_ms = s_last_activity_ms;
        if (s_state_lock) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            last_activity_ms = s_last_activity_ms;
            xSemaphoreGive(s_state_lock);
        }
        if (s_emotion) {
            uint64_t interaction_ms = (uint64_t)s_emotion->last_interaction_ms;
            // Ignore future timestamps caused by concurrent task reads around millisecond boundaries.
            if (interaction_ms <= now_ms && interaction_ms > last_activity_ms) {
                last_activity_ms = interaction_ms;
            }
            if (interaction_ms > s_last_seen_interaction_ms) {
                s_last_seen_interaction_ms = (uint32_t)interaction_ms;
            }
        }
        uint64_t inactive_ms = elapsed_ms_u64(last_activity_ms, now_ms);
        bool inactivity_grace = elapsed_ms_u64(s_last_sleep_transition_ms, now_ms) < k_post_wake_inactivity_grace_ms;

        int local_hour = 0;
        bool within_sleep_window = get_local_hour_if_valid(&local_hour) && is_in_sleep_window(local_hour);
        bool is_overfed_sleepy = (s_emotion &&
                                  s_emotion->current_state == EMOTION_STATE_SLEEPY &&
                                  s_emotion->hunger > 100);

        // Keep sleep glyph reserved for actual sleep-system context:
        // the active sleep window or an active sleep session. Do not show it
        // for generic "sleepy" mood alone, otherwise it can remain visible for
        // long stretches without any real sleep transition pending.
        if (s_glyph_inited) {
            if (within_sleep_window) {
                eldra_glyphs_show(GLYPH_SLEEP, 0);
            } else {
                eldra_glyphs_hide_unforced();
            }
        }

        // Condition 2: battery at or below 10%.
        if (s_emotion && s_emotion->battery_percent <= s_low_battery_pct) {
            EL_LOGI(TAG, "Auto sleep: low battery (%u%%)", (unsigned)s_emotion->battery_percent);
            sleep_now_internal(false);
            return;
        }

        // Condition 3: no activity for 30 minutes (any time).
        if (!inactivity_grace && inactive_ms >= s_global_inactivity_ms) {
            EL_LOGI(TAG, "Auto sleep: global inactivity timeout (%llums)", (unsigned long long)inactive_ms);
            sleep_now_internal(false);
            return;
        }

        // Condition 4: over-fed sleepy state.
        if (!inactivity_grace && is_overfed_sleepy) {
            // Require sustained inactivity for overfed sleep escalation so handling
            // or gentle play does not force sleep unexpectedly.
            if (inactive_ms >= s_window_inactivity_ms) {
                if (s_overfed_sleepy_since_ms == 0) {
                    s_overfed_sleepy_since_ms = now_ms;
                    EL_LOGI(TAG, "Overfed sleepy + inactivity entered; starting hold timer");
                }
                if ((now_ms - s_overfed_sleepy_since_ms) >= s_overfed_hold_ms) {
                    EL_LOGI(TAG, "Auto sleep: overfed sleepy hold reached");
                    sleep_now_internal(false);
                    return;
                }
            } else {
                s_overfed_sleepy_since_ms = 0;
            }
        } else {
            s_overfed_sleepy_since_ms = 0;
        }

        // Condition 1: inside sleep window and inactive for configured duration.
        if (!inactivity_grace && within_sleep_window) {
            if (inactive_ms >= s_window_inactivity_ms) {
                EL_LOGI(TAG, "Auto sleep: inactivity timeout (%llums)", (unsigned long long)inactive_ms);
                sleep_now_internal(true);
                return;
            }
        }
        return;
    }

    if (s_glyph_inited) {
        eldra_glyphs_show(GLYPH_SLEEP, 0);
    }

    if (s_emotion) {
        uint32_t interaction_ms = s_emotion->last_interaction_ms;
        if (interaction_ms > s_last_seen_interaction_ms) {
            s_last_seen_interaction_ms = interaction_ms;
            if (interaction_ms <= now_ms &&
                elapsed_ms_u64(s_sleep_started_ms, now_ms) >= k_wake_motion_guard_ms &&
                elapsed_ms_u64(interaction_ms, now_ms) < 3000ULL) {
                EL_LOGI(TAG, "Wake condition: interaction detected (%u ms ago)",
                         (unsigned)elapsed_ms_u64(interaction_ms, now_ms));
                eldra_sleep_wake_now();
                return;
            }
        }
    }

    int local_hour = 0;
    if (snapshot_sleep_reason_window() && get_local_hour_if_valid(&local_hour)) {
        if (!is_in_sleep_window(local_hour)) {
            EL_LOGI(TAG, "Wake condition: sleep window ended at hour=%d", local_hour);
            eldra_sleep_wake_now();
            return;
        }
    }

    if (!power_stage_done && shutdown_at_ms > 0 && now_ms >= shutdown_at_ms) {
        if (s_state_lock) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            if (s_sleeping && !s_power_stage_done && now_ms >= s_shutdown_time_ms) {
                s_power_stage_done = true;
                post_sleep_msg(SLEEP_MSG_SHUTDOWN);
                EL_LOGW(TAG, "Sleep window reached; staging power-down now");
            }
            xSemaphoreGive(s_state_lock);
        }
    }
}

bool eldra_sleep_on_motion(float ax_g, float ay_g, float az_g, float gx_dps, float gy_dps, float gz_dps)
{
    float amag = sqrtf(ax_g * ax_g + ay_g * ay_g + az_g * az_g);
    float gdev = fabsf(amag - 1.0f);
    float gyro = sqrtf(gx_dps * gx_dps + gy_dps * gy_dps + gz_dps * gz_dps);

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    bool sleeping = false;
    bool motion_candidate = (gdev > k_activity_gdev_thresh || gyro > k_activity_gyro_thresh_dps);
    bool strong_motion = (gdev > k_activity_strong_gdev_thresh || gyro > k_activity_strong_gyro_thresh_dps);
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        sleeping = s_sleeping;
        if (strong_motion) {
            s_last_activity_ms = now_ms;
            s_motion_qualify_since_ms = now_ms;
        } else if (motion_candidate) {
            if (s_motion_qualify_since_ms == 0) {
                s_motion_qualify_since_ms = now_ms;
            } else if (elapsed_ms_u64(s_motion_qualify_since_ms, now_ms) >= k_activity_qualify_ms) {
                s_last_activity_ms = now_ms;
            }
        } else {
            s_motion_qualify_since_ms = 0;
        }
        xSemaphoreGive(s_state_lock);
    } else {
        sleeping = s_sleeping;
    }

    if (!sleeping) {
        return false;
    }
    if (elapsed_ms_u64(s_sleep_started_ms, now_ms) < k_wake_motion_guard_ms) {
        return true;
    }

    if (gdev > k_wake_gdev_thresh || gyro > k_wake_gyro_thresh_dps) {
        EL_LOGI(TAG, "Wake condition: shake detected (gdev=%.2f gyro=%.1f)", (double)gdev, (double)gyro);
        eldra_sleep_wake_now();
        return true;
    }
    return true;
}

bool eldra_sleep_get_shutdown_eta_ms(uint64_t now_ms, uint32_t *remaining_ms)
{
    bool active = false;
    uint32_t rem = 0;

    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        if (s_sleeping && !s_power_stage_done && s_shutdown_time_ms > 0) {
            if (s_shutdown_time_ms > now_ms) {
                uint64_t delta = s_shutdown_time_ms - now_ms;
                rem = (delta > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)delta;
            } else {
                rem = 0;
            }
            active = true;
        }
        xSemaphoreGive(s_state_lock);
    }

    if (remaining_ms) {
        *remaining_ms = rem;
    }
    return active;
}

bool eldra_sleep_should_suppress_imu(uint64_t now_ms)
{
    bool sleeping = false;
    uint64_t last_transition_ms = 0;

    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        sleeping = s_sleeping;
        last_transition_ms = s_last_sleep_transition_ms;
        xSemaphoreGive(s_state_lock);
    } else {
        sleeping = s_sleeping;
        last_transition_ms = s_last_sleep_transition_ms;
    }

    if (sleeping) {
        return true;
    }
    return elapsed_ms_u64(last_transition_ms, now_ms) < k_post_wake_imu_guard_ms;
}

bool eldra_sleep_is_active(void)
{
    return snapshot_sleeping();
}

