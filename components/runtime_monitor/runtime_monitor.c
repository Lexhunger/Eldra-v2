#include "runtime_monitor.h"

#include <stdio.h>
#include <string.h>

#include "eldra_comms.h"
#include "eldra_display_round.h"
#include "eldra_logging.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_driver.h"

static const char *TAG = "runtime_mon";
static const TickType_t k_lock_wait_ticks = pdMS_TO_TICKS(2);
static const uint32_t k_loop_period_ms = 2000;

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_monitor_task = NULL;
static TaskHandle_t s_main_task = NULL;
static TaskHandle_t *s_display_task_ptr = NULL;
static volatile uint32_t *s_main_hb_ms = NULL;
static volatile uint32_t *s_display_hb_ms = NULL;
static runtime_monitor_config_t s_cfg = {0};
static runtime_monitor_stats_t s_stats = {0};

static inline bool lock_take(void)
{
    if (!s_lock) {
        return false;
    }
    return (xSemaphoreTake(s_lock, k_lock_wait_ticks) == pdTRUE);
}

static inline void lock_give(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void runtime_monitor_get_default_config(runtime_monitor_config_t *out_cfg)
{
    if (!out_cfg) {
        return;
    }
    out_cfg->snapshot_interval_ms = 60000;
    out_cfg->sd_snapshot_enabled = true;
    out_cfg->console_snapshot_enabled = false;
    out_cfg->stall_threshold_ms = 60000;
    out_cfg->stall_hits_before_restart = 3;
    out_cfg->auto_restart_on_stall = true;
}

void runtime_monitor_get_config(runtime_monitor_config_t *out_cfg)
{
    if (!out_cfg) {
        return;
    }
    if (!lock_take()) {
        runtime_monitor_get_default_config(out_cfg);
        return;
    }
    *out_cfg = s_cfg;
    lock_give();
}

void runtime_monitor_set_config(const runtime_monitor_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    runtime_monitor_config_t next = *cfg;
    if (next.stall_hits_before_restart == 0) {
        next.stall_hits_before_restart = 1;
    }
    if (next.snapshot_interval_ms != 0 && next.snapshot_interval_ms < 5000) {
        next.snapshot_interval_ms = 5000;
    }
    if (next.stall_threshold_ms < 1000) {
        next.stall_threshold_ms = 1000;
    }

    if (!lock_take()) {
        return;
    }
    s_cfg = next;
    lock_give();
}

void runtime_monitor_get_stats(runtime_monitor_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    if (!lock_take()) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    *out_stats = s_stats;
    lock_give();
}

static void snapshot_metrics(uint32_t now_ms, const runtime_monitor_config_t *cfg)
{
    comms_stats_t comms = {0};
    comms_get_stats(&comms);

    eldra_display_round_diag_t panel = {0};
    eldra_display_round_get_diag(&panel);

    TaskHandle_t disp_task = (s_display_task_ptr != NULL) ? *s_display_task_ptr : NULL;
    uint32_t main_hwm = (s_main_task != NULL) ? (uint32_t)uxTaskGetStackHighWaterMark(s_main_task) : 0;
    uint32_t disp_hwm = (disp_task != NULL) ? (uint32_t)uxTaskGetStackHighWaterMark(disp_task) : 0;
    uint32_t mon_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);

    uint32_t free_int = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t min_int = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_ps = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t min_ps = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    uint32_t main_age = 0;
    uint32_t disp_age = 0;
    if (s_main_hb_ms && *s_main_hb_ms != 0 && now_ms >= *s_main_hb_ms) {
        main_age = now_ms - *s_main_hb_ms;
    }
    if (s_display_hb_ms && *s_display_hb_ms != 0 && now_ms >= *s_display_hb_ms) {
        disp_age = now_ms - *s_display_hb_ms;
    }

    char line[256];
    snprintf(line, sizeof(line),
             "perf hb_age(ms)=main:%u disp:%u bridge(depth=%u max=%u dropF=%u dropL=%u lat=%u/%u/%u) panel(fail=%u to=%u sync=%u reset=%u) heap(int=%u/%u ps=%u/%u) stack(words m/d/mon=%u/%u/%u)",
             (unsigned)main_age, (unsigned)disp_age,
             (unsigned)comms.queue_depth, (unsigned)comms.queue_max_depth,
             (unsigned)comms.enqueue_drop_full, (unsigned)comms.enqueue_drop_lock,
             (unsigned)comms.dequeue_latency_last_ms, (unsigned)comms.dequeue_latency_avg_ms,
             (unsigned)comms.dequeue_latency_max_ms,
             (unsigned)panel.blit_fail_count, (unsigned)panel.blit_timeout_count,
             (unsigned)panel.sync_count, (unsigned)panel.reset_count,
             (unsigned)free_int, (unsigned)min_int, (unsigned)free_ps, (unsigned)min_ps,
             (unsigned)main_hwm, (unsigned)disp_hwm, (unsigned)mon_hwm);

    if (cfg->sd_snapshot_enabled) {
        (void)sd_driver_log_async(line);
    }
    if (cfg->console_snapshot_enabled) {
        EL_LOGI(TAG, "%s", line);
    }

    if (lock_take()) {
        s_stats.last_snapshot_ms = now_ms;
        s_stats.snapshot_count++;
        s_stats.main_stack_hwm_words = main_hwm;
        s_stats.display_stack_hwm_words = disp_hwm;
        s_stats.monitor_stack_hwm_words = mon_hwm;
        s_stats.free_heap_internal = free_int;
        s_stats.min_heap_internal = min_int;
        s_stats.free_heap_spiram = free_ps;
        s_stats.min_heap_spiram = min_ps;
        s_stats.bridge_queue_depth = comms.queue_depth;
        s_stats.bridge_queue_max_depth = comms.queue_max_depth;
        s_stats.bridge_drop_full = comms.enqueue_drop_full;
        s_stats.bridge_drop_lock = comms.enqueue_drop_lock;
        s_stats.bridge_dequeue_latency_last_ms = comms.dequeue_latency_last_ms;
        s_stats.bridge_dequeue_latency_avg_ms = comms.dequeue_latency_avg_ms;
        s_stats.bridge_dequeue_latency_max_ms = comms.dequeue_latency_max_ms;
        s_stats.panel_blit_fail_count = panel.blit_fail_count;
        s_stats.panel_blit_timeout_count = panel.blit_timeout_count;
        s_stats.panel_sync_count = panel.sync_count;
        s_stats.panel_reset_count = panel.reset_count;
        lock_give();
    }
}

static void monitor_task(void *arg)
{
    (void)arg;
    uint8_t main_stall_hits = 0;
    uint8_t disp_stall_hits = 0;
    uint32_t last_snapshot_ms = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(k_loop_period_ms));
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        runtime_monitor_config_t cfg;
        runtime_monitor_get_config(&cfg);

        if (s_main_hb_ms && *s_main_hb_ms != 0 && (now_ms - *s_main_hb_ms) > cfg.stall_threshold_ms) {
            main_stall_hits++;
            if (lock_take()) {
                s_stats.main_stall_hits = main_stall_hits;
                lock_give();
            }
            EL_LOGW(TAG, "Main heartbeat stale (%u ms) hit=%u/%u",
                    (unsigned)(now_ms - *s_main_hb_ms),
                    (unsigned)main_stall_hits,
                    (unsigned)cfg.stall_hits_before_restart);
            if (cfg.auto_restart_on_stall && main_stall_hits >= cfg.stall_hits_before_restart) {
                if (lock_take()) {
                    s_stats.stall_restarts++;
                    lock_give();
                }
                EL_LOGE(TAG, "Main loop stalled repeatedly; restarting");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            main_stall_hits = 0;
            if (lock_take()) {
                s_stats.main_stall_hits = 0;
                lock_give();
            }
        }

        TaskHandle_t disp_task = (s_display_task_ptr != NULL) ? *s_display_task_ptr : NULL;
        if (disp_task && s_display_hb_ms && *s_display_hb_ms != 0 &&
            (now_ms - *s_display_hb_ms) > cfg.stall_threshold_ms) {
            disp_stall_hits++;
            if (lock_take()) {
                s_stats.display_stall_hits = disp_stall_hits;
                lock_give();
            }
            EL_LOGW(TAG, "Display heartbeat stale (%u ms) hit=%u/%u",
                    (unsigned)(now_ms - *s_display_hb_ms),
                    (unsigned)disp_stall_hits,
                    (unsigned)cfg.stall_hits_before_restart);
            if (cfg.auto_restart_on_stall && disp_stall_hits >= cfg.stall_hits_before_restart) {
                if (lock_take()) {
                    s_stats.stall_restarts++;
                    lock_give();
                }
                EL_LOGE(TAG, "Display loop stalled repeatedly; restarting");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            disp_stall_hits = 0;
            if (lock_take()) {
                s_stats.display_stall_hits = 0;
                lock_give();
            }
        }

        if (cfg.snapshot_interval_ms > 0 && (now_ms - last_snapshot_ms) >= cfg.snapshot_interval_ms) {
            snapshot_metrics(now_ms, &cfg);
            last_snapshot_ms = now_ms;
        }
    }
}

esp_err_t runtime_monitor_start(const runtime_monitor_config_t *cfg,
                                volatile uint32_t *main_heartbeat_ms,
                                volatile uint32_t *display_heartbeat_ms,
                                TaskHandle_t main_task_handle,
                                TaskHandle_t *display_task_handle_ptr)
{
    if (!cfg || !main_heartbeat_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_monitor_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    runtime_monitor_set_config(cfg);
    s_main_hb_ms = main_heartbeat_ms;
    s_display_hb_ms = display_heartbeat_ms;
    s_main_task = main_task_handle;
    s_display_task_ptr = display_task_handle_ptr;
    memset(&s_stats, 0, sizeof(s_stats));

    BaseType_t ok = xTaskCreatePinnedToCore(monitor_task, "runtime_mon", 3072, NULL, 2, &s_monitor_task, 1);
    if (ok != pdPASS) {
        s_monitor_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    EL_LOGI(TAG, "Runtime monitor started (snapshot=%ums stall=%ums hits=%u auto_restart=%d sd=%d console=%d)",
            (unsigned)cfg->snapshot_interval_ms,
            (unsigned)cfg->stall_threshold_ms,
            (unsigned)cfg->stall_hits_before_restart,
            cfg->auto_restart_on_stall ? 1 : 0,
            cfg->sd_snapshot_enabled ? 1 : 0,
            cfg->console_snapshot_enabled ? 1 : 0);
    return ESP_OK;
}
