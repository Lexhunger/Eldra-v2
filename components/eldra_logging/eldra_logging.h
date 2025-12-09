#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

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
 * @brief Dump recent buffered logs to the current log sink (console).
 * @param tag_filter Optional tag filter (NULL for all); should match the tag argument used in log_event.
 * @param min_level Minimum level to include.
 * @param limit Max number of entries to print (0 uses a default cap).
 */
void log_dump_recent(const char *tag_filter, log_level_t min_level, size_t limit);
