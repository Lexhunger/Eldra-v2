#include "eldra_logging.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"

#define LOG_RING_SIZE 100
#define LOG_TAG_MAX 16
#define LOG_MSG_MAX 96

typedef struct {
    uint64_t ts_us;
    log_level_t level;
    char tag[LOG_TAG_MAX];
    char msg[LOG_MSG_MAX];
} log_entry_t;

static log_entry_t s_ring[LOG_RING_SIZE];
static size_t s_ring_head = 0;
static size_t s_ring_count = 0;

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
    s_ring_head = 0;
    s_ring_count = 0;
    memset(s_ring, 0, sizeof(s_ring));
}

void log_event(log_level_t level, const char *tag, const char *fmt, ...) {
    const char *log_tag = (tag != NULL) ? tag : "eldra";
    va_list args;
    va_start(args, fmt);
    esp_log_level_t esp_level = to_esp_level(level);
    esp_log_writev(esp_level, log_tag, fmt, args);
    va_end(args);

    // Copy into ring buffer for later console retrieval.
    log_entry_t *slot = &s_ring[s_ring_head];
    slot->ts_us = esp_timer_get_time();
    slot->level = level;
    strncpy(slot->tag, log_tag, LOG_TAG_MAX - 1);
    slot->tag[LOG_TAG_MAX - 1] = '\0';
    va_start(args, fmt);
    vsnprintf(slot->msg, LOG_MSG_MAX, fmt, args);
    va_end(args);
    s_ring_head = (s_ring_head + 1U) % LOG_RING_SIZE;
    if (s_ring_count < LOG_RING_SIZE) {
        s_ring_count++;
    }

    // TODO: Mirror this message to SD card and optionally to the home server.
    (void)printf; // Silence unused warnings if ESP logging redirects output.
}

void log_dump_recent(const char *tag_filter, log_level_t min_level, size_t limit) {
    size_t to_print = (limit == 0) ? 100 : limit;
    if (to_print > s_ring_count) {
        to_print = s_ring_count;
    }
    size_t idx = (s_ring_head + LOG_RING_SIZE - s_ring_count) % LOG_RING_SIZE;
    for (size_t i = 0; i < s_ring_count && to_print > 0; ++i) {
        const log_entry_t *e = &s_ring[(idx + i) % LOG_RING_SIZE];
        if (e->level < min_level) {
            continue;
        }
        if (tag_filter && tag_filter[0] != '\0' && strcasecmp(tag_filter, e->tag) != 0) {
            continue;
        }
        uint32_t ms = (uint32_t)(e->ts_us / 1000ULL);
        esp_log_level_t esp_level = to_esp_level(e->level);
        esp_log_write(esp_level, e->tag, "%10u ms | %s", (unsigned)ms, e->msg);
        to_print--;
    }
}
