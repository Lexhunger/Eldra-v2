#include "eldra_logging.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LOG_RING_SIZE 100
#define LOG_TAG_MAX 16
#define LOG_MSG_MAX 96
#define LOG_FILE_PATH "/sdcard/eldra.log"
#define SD_RETRY_INTERVAL_MS 5000U
#define SD_FLUSH_INTERVAL_MS 2000U
#define SD_FLUSH_PENDING 8U
#define LOG_TZ_OFFSET_SEC (-5 * 3600) // EST (fixed, no DST)

typedef struct {
    uint64_t ts_us;
    log_level_t level;
    char tag[LOG_TAG_MAX];
    char msg[LOG_MSG_MAX];
} log_entry_t;

static log_entry_t s_ring[LOG_RING_SIZE];
static size_t s_ring_head = 0;
static size_t s_ring_count = 0;
static FILE *s_log_file = NULL;
static bool s_log_file_attempted = false;
static int s_last_log_date = -1;
static bool s_sd_enabled = true;
static log_level_t s_sd_min_level = LOG_LEVEL_INFO;
static log_level_t s_console_min_level = LOG_LEVEL_INFO;
static uint32_t s_last_retry_ms = 0;
static uint32_t s_last_flush_ms = 0;
static uint32_t s_pending_lines = 0;
static uint32_t s_console_level_decay_ms = 0;
static log_level_t s_console_level_target = LOG_LEVEL_INFO;
static TaskHandle_t s_log_task_handle = NULL;
static bool s_logged_open_success = false;
static bool s_logged_open_fail = false;
static const char *LOG_INTERNAL_TAG = "eldra_logging";
static char s_prompt[16] = {0};
static bool s_prompt_enabled = false;

static void log_background_task(void *param);

typedef struct {
    const char *tag;
    uint32_t every_n;
    uint32_t counter;
} log_sample_rule_t;

static log_sample_rule_t s_sample_rules[] = {
    {"eldra_eyes", 5, 0}, // throttle chatty eye logs: keep 1 of 5
};

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

static void format_date_from_ms(uint32_t ms_since_boot, char *out_date, size_t len) {
    time_t seconds = (time_t)(ms_since_boot / 1000U) + LOG_TZ_OFFSET_SEC;
    struct tm tm_out;
    gmtime_r(&seconds, &tm_out);
    snprintf(out_date, len, "%04d%02d%02d", tm_out.tm_year + 1900, tm_out.tm_mon + 1, tm_out.tm_mday);
}

static void rotate_log_if_needed(uint32_t ms) {
    char date_str[16] = {0};
    format_date_from_ms(ms, date_str, sizeof(date_str));
    int date_val = atoi(date_str);
    if (date_val == s_last_log_date && s_log_file) {
        return;
    }
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/eldra_%s.log", date_str);
    s_log_file = fopen(path, "a");
    if (s_log_file) {
        setvbuf(s_log_file, NULL, _IOFBF, 1024);
        ESP_LOGI(LOG_INTERNAL_TAG, "SD log file opened: %s", path);
        s_logged_open_success = true;
        s_logged_open_fail = false;
    } else if (!s_logged_open_fail) {
        ESP_LOGW(LOG_INTERNAL_TAG, "Failed to open log file %s (errno=%d)", path, errno);
        s_logged_open_fail = true;
    }
    s_last_log_date = date_val;
    s_pending_lines = 0;
    s_last_flush_ms = ms;
}

static bool sd_sample_allows(const char *tag) {
    if (!tag) {
        return true;
    }
    for (size_t i = 0; i < sizeof(s_sample_rules) / sizeof(s_sample_rules[0]); ++i) {
        if (strcasecmp(tag, s_sample_rules[i].tag) == 0) {
            s_sample_rules[i].counter++;
            return (s_sample_rules[i].counter % s_sample_rules[i].every_n) == 0;
        }
    }
    return true;
}

static void sd_try_open(uint32_t ms) {
    if (!s_sd_enabled) {
        return;
    }
    if (s_log_file) {
        return;
    }
    if (s_log_file_attempted && (ms - s_last_retry_ms) < SD_RETRY_INTERVAL_MS) {
        return;
    }
    rotate_log_if_needed(ms);
    s_last_retry_ms = ms;
    s_log_file_attempted = true;
}

static void sd_maybe_flush(uint32_t ms) {
    if (s_log_file == NULL) {
        return;
    }
    if (s_pending_lines >= SD_FLUSH_PENDING || (ms - s_last_flush_ms) >= SD_FLUSH_INTERVAL_MS) {
        fflush(s_log_file);
        s_pending_lines = 0;
        s_last_flush_ms = ms;
    }
}

static void format_timestamp(char *out, size_t len) {
    time_t now_epoch = time(NULL);
    if (now_epoch > 0) {
        struct tm tm_out;
        time_t shifted = now_epoch + LOG_TZ_OFFSET_SEC;
        gmtime_r(&shifted, &tm_out);
        snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02d EST",
                 tm_out.tm_year + 1900, tm_out.tm_mon + 1, tm_out.tm_mday,
                 tm_out.tm_hour, tm_out.tm_min, tm_out.tm_sec);
    } else {
        snprintf(out, len, "unknown-time");
    }
}

void log_init(void) {
    // Placeholder for SD card or remote logging activation.
    s_ring_head = 0;
    s_ring_count = 0;
    memset(s_ring, 0, sizeof(s_ring));
    s_log_file = NULL;
    s_log_file_attempted = false;
    s_last_log_date = -1;
    s_sd_enabled = true;
    s_sd_min_level = LOG_LEVEL_INFO;
    s_console_min_level = LOG_LEVEL_ERROR; // default: keep console quiet; use LOGS/LOGLEVEL to view
    s_last_retry_ms = 0;
    s_last_flush_ms = 0;
    s_pending_lines = 0;
    s_console_level_decay_ms = 0;
    s_console_level_target = LOG_LEVEL_ERROR;
    s_logged_open_success = false;
    s_logged_open_fail = false;
    s_prompt_enabled = false;
    s_prompt[0] = '\0';
    for (size_t i = 0; i < sizeof(s_sample_rules) / sizeof(s_sample_rules[0]); ++i) {
        s_sample_rules[i].counter = 0;
    }

    if (s_log_task_handle == NULL) {
        xTaskCreatePinnedToCore(log_background_task, "log_bg", 2048, NULL, 2, &s_log_task_handle, 0);
    }
}

void log_event(log_level_t level, const char *tag, const char *fmt, ...) {
    const char *log_tag = (tag != NULL) ? tag : "eldra";
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);
    if (level >= s_console_min_level) {
        esp_log_level_t esp_level = to_esp_level(level);
        char console_buf[LOG_MSG_MAX + 2];
        int n = vsnprintf(console_buf, sizeof(console_buf), fmt, args);
        if (n < 0 || n >= (int)sizeof(console_buf)) {
            // Truncated; ensure null-termination
            console_buf[sizeof(console_buf) - 1] = '\0';
        }
        // Ensure each console line ends with a newline to avoid concatenation.
        size_t len = strlen(console_buf);
        if (len + 1 < sizeof(console_buf)) {
            console_buf[len] = '\n';
            console_buf[len + 1] = '\0';
        }
        esp_log_write(esp_level, log_tag, "%s", console_buf);
        if (s_prompt_enabled && s_prompt[0]) {
            // Re-print prompt so REPL doesn't require an extra Enter after async logs.
            // Use stdout directly to avoid extra log prefixes.
            fputs(s_prompt, stdout);
            fflush(stdout);
        }
    }
    va_end(args);

    // Copy into ring buffer for later console/HTTP retrieval.
    log_entry_t *slot = &s_ring[s_ring_head];
    slot->ts_us = esp_timer_get_time();
    slot->level = level;
    strncpy(slot->tag, log_tag, LOG_TAG_MAX - 1);
    slot->tag[LOG_TAG_MAX - 1] = '\0';
    vsnprintf(slot->msg, LOG_MSG_MAX, fmt, args_copy);
    va_end(args_copy);
    s_ring_head = (s_ring_head + 1U) % LOG_RING_SIZE;
    if (s_ring_count < LOG_RING_SIZE) {
        s_ring_count++;
    }

    // Append to SD log file if available. Attempt lazily and rotate daily (UTC).
    uint32_t ms = (uint32_t)(slot->ts_us / 1000ULL);
    if (s_sd_enabled) {
        sd_try_open(ms);
        if (s_log_file) {
            rotate_log_if_needed(ms);
            if (level >= s_sd_min_level && sd_sample_allows(log_tag)) {
                char ts[32];
                format_timestamp(ts, sizeof(ts));
                fprintf(s_log_file, "%s %10u ms [%s] %s\n", ts, (unsigned)ms, slot->tag, slot->msg);
                s_pending_lines++;
                sd_maybe_flush(ms);
            }
        }
    }
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
        esp_log_write(esp_level, e->tag, "%10u ms | %s\n", (unsigned)ms, e->msg);
        to_print--;
    }
}

void log_sd_set_enabled(bool enabled) {
    s_sd_enabled = enabled;
    if (!enabled && s_log_file) {
        fflush(s_log_file);
        fclose(s_log_file);
        s_log_file = NULL;
    }
}

void log_sd_force_rotate(void) {
    if (s_log_file) {
        fflush(s_log_file);
        fclose(s_log_file);
        s_log_file = NULL;
    }
    s_last_log_date = -1;
    s_log_file_attempted = false;
}

void log_sd_notify_mounted(void) {
    // Reset retry bookkeeping and try immediately.
    s_log_file_attempted = false;
    s_logged_open_success = false;
    s_logged_open_fail = false;
    s_last_retry_ms = 0;
    s_last_log_date = -1;
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    sd_try_open(ms);
}

void log_set_console_level(log_level_t level) {
    s_console_min_level = level;
}

void log_set_console_level_temporary(log_level_t level, uint32_t duration_ms, log_level_t revert_level) {
    s_console_min_level = level;
    s_console_level_target = revert_level;
    s_console_level_decay_ms = (duration_ms > 0) ? (uint32_t)(esp_timer_get_time() / 1000ULL) + duration_ms : 0;
}

void log_task_start(void) {
    // Task is created in log_init if missing; this function can be used to ensure start.
    if (s_log_task_handle == NULL) {
        log_init();
    }
}

void log_set_prompt(const char *prompt) {
    if (prompt && prompt[0]) {
        strlcpy(s_prompt, prompt, sizeof(s_prompt));
        s_prompt_enabled = true;
    } else {
        s_prompt_enabled = false;
        s_prompt[0] = '\0';
    }
}

static void log_background_task(void *param) {
    (void)param;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        sd_try_open(ms);
        if (s_log_file) {
            rotate_log_if_needed(ms);
            sd_maybe_flush(ms);
        }
    }
}
