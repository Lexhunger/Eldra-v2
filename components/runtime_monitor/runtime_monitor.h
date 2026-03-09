#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint32_t snapshot_interval_ms;
    bool sd_snapshot_enabled;
    bool console_snapshot_enabled;
    uint32_t stall_threshold_ms;
    uint8_t stall_hits_before_restart;
    bool auto_restart_on_stall;
} runtime_monitor_config_t;

typedef struct {
    uint32_t last_snapshot_ms;
    uint32_t snapshot_count;
    uint32_t main_stall_hits;
    uint32_t display_stall_hits;
    uint32_t stall_restarts;

    uint32_t main_stack_hwm_words;
    uint32_t display_stack_hwm_words;
    uint32_t monitor_stack_hwm_words;

    uint32_t free_heap_internal;
    uint32_t min_heap_internal;
    uint32_t free_heap_spiram;
    uint32_t min_heap_spiram;

    uint32_t bridge_queue_depth;
    uint32_t bridge_queue_max_depth;
    uint32_t bridge_drop_full;
    uint32_t bridge_drop_lock;
    uint32_t bridge_dequeue_latency_last_ms;
    uint32_t bridge_dequeue_latency_avg_ms;
    uint32_t bridge_dequeue_latency_max_ms;

    uint32_t panel_blit_fail_count;
    uint32_t panel_blit_timeout_count;
    uint32_t panel_sync_count;
    uint32_t panel_reset_count;
} runtime_monitor_stats_t;

void runtime_monitor_get_default_config(runtime_monitor_config_t *out_cfg);

esp_err_t runtime_monitor_start(const runtime_monitor_config_t *cfg,
                                volatile uint32_t *main_heartbeat_ms,
                                volatile uint32_t *display_heartbeat_ms,
                                TaskHandle_t main_task_handle,
                                TaskHandle_t *display_task_handle_ptr);

void runtime_monitor_get_config(runtime_monitor_config_t *out_cfg);
void runtime_monitor_set_config(const runtime_monitor_config_t *cfg);
void runtime_monitor_get_stats(runtime_monitor_stats_t *out_stats);
