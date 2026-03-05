#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @file eldra_logging.h
 * @brief Lightweight logging wrapper so modules can be rerouted to SD/server later without code churn.
 */

/**
 * @brief Log severity levels.
 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} log_level_t;

/**
 * @brief Runtime snapshot for a discovered log tag.
 */
typedef struct {
    char tag[16];
    uint32_t seen_count;
    uint32_t last_seen_ms;
    bool has_console_rule;
    bool console_enabled;
    log_level_t console_min_level;
} log_tag_info_t;

/**
 * @brief Initialize the logging layer. Placeholder for future SD/server wiring.
 */
void log_init(void);

/**
 * @brief Emit a formatted log message.
 * @param level Severity to log.
 * @param tag Subsystem tag (e.g., "emotion", "comms").
 * @param fmt printf-style message.
 * @param ... Format arguments.
 *
 * All modules should prefer this wrapper instead of raw ESP_LOGx calls so we can mirror to SD or a server later.
 */
void log_event(log_level_t level, const char *tag, const char *fmt, ...);

/**
 * @brief Convenience macros for the common log levels.
 */
#define EL_LOGD(tag, fmt, ...) log_event(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#define EL_LOGI(tag, fmt, ...) log_event(LOG_LEVEL_INFO, tag, fmt, ##__VA_ARGS__)
#define EL_LOGW(tag, fmt, ...) log_event(LOG_LEVEL_WARN, tag, fmt, ##__VA_ARGS__)
#define EL_LOGE(tag, fmt, ...) log_event(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)

/**
 * @brief Dump recent buffered logs to the current log sink (console).
 * @param tag_filter Optional tag filter (NULL for all); should match the tag argument used in log_event.
 * @param min_level Minimum level to include.
 * @param limit Max number of entries to print (0 uses a default cap).
 */
void log_dump_recent(const char *tag_filter, log_level_t min_level, size_t limit);

/**
 * @brief Enable/disable SD logging at runtime.
 */
void log_sd_set_enabled(bool enabled);

/**
 * @brief Force a log rotation (e.g., after a date change or manual request).
 */
void log_sd_force_rotate(void);

/**
 * @brief Hint to the logger that SD is mounted so it can retry opening files immediately.
 */
void log_sd_notify_mounted(void);

/**
 * @brief Optional platform hook invoked immediately before SD-backed stdio.
 *
 * The default implementation is a no-op. Platforms can override this with a
 * strong symbol to assert shared-bus guards before file I/O.
 */
void eldra_platform_before_sd_io(void);

/**
 * @brief Optional platform hook invoked immediately after SD-backed stdio.
 *
 * The default implementation is a no-op. Platforms can override this with a
 * strong symbol to restore or reassert shared-bus guards after file I/O.
 */
void eldra_platform_after_sd_io(void);

/**
 * @brief Set the minimum level that is emitted to the console (UART). Ring/SD buffering is unaffected.
 */
void log_set_console_level(log_level_t level);

/**
 * @brief Temporarily raise console level, reverting after duration_ms to revert_level.
 */
void log_set_console_level_temporary(log_level_t level, uint32_t duration_ms, log_level_t revert_level);

/**
 * @brief Set a per-tag console rule. This affects console output only; SD/ring are unchanged.
 *        If enabled=false, the tag is muted on console regardless of global level.
 * @return true when rule was set, false if table full/invalid tag.
 */
bool log_console_set_tag_rule(const char *tag, bool enabled, log_level_t min_level);

/**
 * @brief Clear a per-tag console rule for a specific tag.
 */
void log_console_clear_tag_rule(const char *tag);

/**
 * @brief Clear all per-tag console rules.
 */
void log_console_clear_all_tag_rules(void);

/**
 * @brief Snapshot known tags observed at runtime and their current console routing state.
 * @param out Destination array (can be NULL to query total only).
 * @param max_entries Capacity of out[].
 * @param out_total Optional total number of known tags.
 * @return Number of entries written to out[].
 */
size_t log_get_tag_snapshot(log_tag_info_t *out, size_t max_entries, size_t *out_total);

/**
 * @brief Initialize the logging task (background flush and SD/file handling).
 */
void log_task_start(void);

/**
 * @brief Optional prompt string to reprint after console logs (helps REPL prompt recover after async logs).
 */
void log_set_prompt(const char *prompt);
