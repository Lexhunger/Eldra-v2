#include "eldra_logging.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"

/**
 * @brief Map project log levels to ESP-IDF levels.
 */
static esp_log_level_t to_esp_level(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG:
            return ESP_LOG_DEBUG;
        case LOG_LEVEL_INFO:
            return ESP_LOG_INFO;
        case LOG_LEVEL_WARN:
            return ESP_LOG_WARN;
        case LOG_LEVEL_ERROR:
        default:
            return ESP_LOG_ERROR;
    }
}

void log_init(void) {
    // Placeholder for SD card or remote logging activation.
}

void log_event(log_level_t level, const char *tag, const char *fmt, ...) {
    const char *log_tag = (tag != NULL) ? tag : "eldra";
    va_list args;
    va_start(args, fmt);
    esp_log_level_t esp_level = to_esp_level(level);
    esp_log_writev(esp_level, log_tag, fmt, args);
    va_end(args);

    // TODO: Mirror this message to SD card and optionally to the home server.
    (void)printf; // Silence unused warnings if ESP logging redirects output.
}
