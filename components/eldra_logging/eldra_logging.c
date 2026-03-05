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
#include "freertos/semphr.h"

#define LOG_RING_SIZE 100
#define LOG_TAG_MAX 16
#define LOG_MSG_MAX 160
#define LOG_FILE_DIR "/sdcard/LOGS"
#define SD_RETRY_INTERVAL_MS 5000U
#define SD_FLUSH_INTERVAL_MS 2000U
#define SD_FLUSH_PENDING 8U
#define CONSOLE_TAG_RULE_MAX 24
#define KNOWN_TAG_MAX 64
static const time_t k_valid_log_epoch_min = 1704067200; // 2024-01-01T00:00:00Z

typedef struct {
    uint64_t ts_us;
    log_level_t level;
    char tag[LOG_TAG_MAX];
    char msg[LOG_MSG_MAX];
} log_entry_t;

typedef struct {
    bool used;
    bool enabled;
    log_level_t min_level;
    char tag[LOG_TAG_MAX];
} console_tag_rule_t;

typedef struct {
    bool used;
    char tag[LOG_TAG_MAX];
    uint32_t seen_count;
    uint64_t last_seen_us;
    log_level_t last_level;
} known_tag_t;

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
static SemaphoreHandle_t s_log_lock = NULL;
static const TickType_t k_log_lock_wait = pdMS_TO_TICKS(5);
static console_tag_rule_t s_console_tag_rules[CONSOLE_TAG_RULE_MAX];
static known_tag_t s_known_tags[KNOWN_TAG_MAX];

void __attribute__((weak)) eldra_platform_before_sd_io(void) {}
void __attribute__((weak)) eldra_platform_after_sd_io(void) {}

static inline bool log_lock(void)
{
    if (!s_log_lock) {
        return true;
    }
    return (xSemaphoreTake(s_log_lock, k_log_lock_wait) == pdTRUE);
}

static inline void log_unlock(void)
{
    if (s_log_lock) {
        xSemaphoreGive(s_log_lock);
    }
}

static inline void sd_io_begin(void)
{
    eldra_platform_before_sd_io();
}

static inline void sd_io_end(void)
{
    eldra_platform_after_sd_io();
}

static void log_background_task(void *param);

typedef struct {
    const char *tag;
    uint32_t every_n;
    uint32_t counter;
} log_sample_rule_t;

static log_sample_rule_t s_sample_rules[] = {
    {"eldra_eyes", 5, 0}, // throttle chatty eye logs: keep 1 of 5
};

static bool console_mute_tag(const char *tag)
{
    if (!tag) {
        return false;
    }
    // Keep verbose glyph breadcrumbs in SD/ring only, not UART console.
    return strcasecmp(tag, "eldra_glyphs") == 0;
}

static int find_console_tag_rule(const char *tag)
{
    if (!tag || !tag[0]) {
        return -1;
    }
    for (int i = 0; i < CONSOLE_TAG_RULE_MAX; ++i) {
        if (!s_console_tag_rules[i].used) {
            continue;
        }
        if (strcasecmp(s_console_tag_rules[i].tag, tag) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_known_tag(const char *tag)
{
    if (!tag || !tag[0]) {
        return -1;
    }
    for (int i = 0; i < KNOWN_TAG_MAX; ++i) {
        if (!s_known_tags[i].used) {
            continue;
        }
        if (strcasecmp(s_known_tags[i].tag, tag) == 0) {
            return i;
        }
    }
    return -1;
}

static void note_known_tag(const char *tag, log_level_t level, uint64_t ts_us)
{
    if (!tag || !tag[0]) {
        return;
    }
    int idx = find_known_tag(tag);
    if (idx < 0) {
        for (int i = 0; i < KNOWN_TAG_MAX; ++i) {
            if (!s_known_tags[i].used) {
                idx = i;
                s_known_tags[i].used = true;
                strlcpy(s_known_tags[i].tag, tag, sizeof(s_known_tags[i].tag));
                s_known_tags[i].seen_count = 0;
                s_known_tags[i].last_seen_us = 0;
                s_known_tags[i].last_level = LOG_LEVEL_INFO;
                break;
            }
        }
    }
    if (idx < 0) {
        // Table full: keep existing entries, ignore new tags.
        return;
    }
    s_known_tags[idx].seen_count++;
    s_known_tags[idx].last_seen_us = ts_us;
    s_known_tags[idx].last_level = level;
}

static bool console_should_emit(const char *tag, log_level_t level)
{
    int idx = find_console_tag_rule(tag);
    if (idx >= 0) {
        if (!s_console_tag_rules[idx].enabled) {
            return false;
        }
        return (level >= s_console_tag_rules[idx].min_level);
    }
    if (console_mute_tag(tag)) {
        return false;
    }
    return (level >= s_console_min_level);
}

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

static void format_date_fallback(uint32_t ms_since_boot, char *out_date, size_t len) {
    uint32_t days = ms_since_boot / (1000U * 86400U);
    snprintf(out_date, len, "uptime%03u", (unsigned)days);
}

static int compute_date_key(time_t now_epoch, uint32_t ms_since_boot, char *out_date, size_t len)
{
    struct tm tm_out = {0};
    if (now_epoch >= k_valid_log_epoch_min && localtime_r(&now_epoch, &tm_out) != NULL) {
        snprintf(out_date, len, "%04d%02d%02d", tm_out.tm_year + 1900, tm_out.tm_mon + 1, tm_out.tm_mday);
        return (tm_out.tm_year + 1900) * 10000 + (tm_out.tm_mon + 1) * 100 + tm_out.tm_mday;
    }

    format_date_fallback(ms_since_boot, out_date, len);
    // Distinguish fallback rollover by uptime day while keeping valid dates positive.
    return -((int)(ms_since_boot / (1000U * 86400U)) + 1);
}

static void rotate_log_if_needed(uint32_t ms) {
    char date_str[16] = {0};
    int date_val = compute_date_key(time(NULL), ms, date_str, sizeof(date_str));
    if (date_val == s_last_log_date && s_log_file) {
        return;
    }
    if (s_log_file) {
        sd_io_begin();
        fclose(s_log_file);
        sd_io_end();
        s_log_file = NULL;
    }
    char path[96];
    snprintf(path, sizeof(path), LOG_FILE_DIR "/eldra_%s.log", date_str);
    sd_io_begin();
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
    sd_io_end();
    s_last_log_date = date_val;
    s_pending_lines = 0;
    s_last_flush_ms = ms;
}

static void format_timestamp(char *out, size_t len) {
    time_t now_epoch = time(NULL);
    if (now_epoch >= k_valid_log_epoch_min) {
        struct tm tm_out;
        if (localtime_r(&now_epoch, &tm_out) != NULL) {
            char tz_buf[12] = {0};
            if (strftime(tz_buf, sizeof(tz_buf), "%Z", &tm_out) == 0) {
                strlcpy(tz_buf, "LOCAL", sizeof(tz_buf));
            }
            snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02d %s",
                     tm_out.tm_year + 1900, tm_out.tm_mon + 1, tm_out.tm_mday,
                     tm_out.tm_hour, tm_out.tm_min, tm_out.tm_sec, tz_buf);
            return;
        }
    }
    snprintf(out, len, "unknown-time");
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
    if (s_pending_lines == 0) {
        return;
    }
    if (s_pending_lines >= SD_FLUSH_PENDING || (ms - s_last_flush_ms) >= SD_FLUSH_INTERVAL_MS) {
        sd_io_begin();
        fflush(s_log_file);
        sd_io_end();
        s_pending_lines = 0;
        s_last_flush_ms = ms;
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
    // Default console level to INFO so mood/battery/etc. remain visible; adjust via LOGLEVEL if needed.
    s_console_min_level = LOG_LEVEL_INFO;
    s_last_retry_ms = 0;
    s_last_flush_ms = 0;
    s_pending_lines = 0;
    s_console_level_decay_ms = 0;
    s_console_level_target = LOG_LEVEL_ERROR;
    s_logged_open_success = false;
    s_logged_open_fail = false;
    s_prompt_enabled = false;
    s_prompt[0] = '\0';
    memset(s_console_tag_rules, 0, sizeof(s_console_tag_rules));
    memset(s_known_tags, 0, sizeof(s_known_tags));
    for (size_t i = 0; i < sizeof(s_sample_rules) / sizeof(s_sample_rules[0]); ++i) {
        s_sample_rules[i].counter = 0;
    }

    if (s_log_lock == NULL) {
        s_log_lock = xSemaphoreCreateMutex();
    }

    if (s_log_task_handle == NULL) {
        // Keep this task roomy: it performs stdio + time formatting + daily rotate.
        // 2KB was too tight under some toolchain/newlib paths and can destabilize
        // around first SNTP sync when log date flips from uptime* to YYYYMMDD.
        xTaskCreatePinnedToCore(log_background_task, "log_bg", 4096, NULL, 2, &s_log_task_handle, 0);
    }
}

void log_event(log_level_t level, const char *tag, const char *fmt, ...) {
    const char *log_tag = (tag != NULL) ? tag : "eldra";
    char msg_buf[LOG_MSG_MAX + 1];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(msg_buf)) {
        msg_buf[sizeof(msg_buf) - 1] = '\0';
    }

    if (s_log_lock == NULL) {
        s_log_lock = xSemaphoreCreateMutex();
    }
    bool locked = log_lock();

    if (console_should_emit(log_tag, level)) {
        esp_log_level_t esp_level = to_esp_level(level);
        char ts[32] = {0};
        format_timestamp(ts, sizeof(ts));
        char console_buf[LOG_MSG_MAX + 48];
        int p = snprintf(console_buf, sizeof(console_buf), "[%s] %s", ts, msg_buf);
        if (p < 0 || p >= (int)sizeof(console_buf)) {
            console_buf[sizeof(console_buf) - 1] = '\0';
        }
        // Ensure each console line ends with a newline to avoid concatenation.
        size_t len = strlen(console_buf);
        if (len + 1 < sizeof(console_buf)) {
            console_buf[len] = '\n';
            console_buf[len + 1] = '\0';
        }
        esp_log_write(esp_level, log_tag, "%s", console_buf);
        // NOTE:
        // Re-printing the prompt here via stdout flush caused occasional USB-serial
        // stalls on target during boot/heavy async logging. Keep logging non-blocking
        // and let the REPL redraw prompt on user input.
    }

    if (!locked) {
        // Fail-soft: do not block real-time tasks if logger lock is contended.
        // We already emitted the console line above (if enabled); skip ring/SD path.
        return;
    }

    // Copy into ring buffer for later console/HTTP retrieval.
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    note_known_tag(log_tag, level, now_us);
    log_entry_t *slot = &s_ring[s_ring_head];
    slot->ts_us = now_us;
    slot->level = level;
    strncpy(slot->tag, log_tag, LOG_TAG_MAX - 1);
    slot->tag[LOG_TAG_MAX - 1] = '\0';
    strlcpy(slot->msg, msg_buf, sizeof(slot->msg));
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
                sd_io_begin();
                fprintf(s_log_file, "%s %10u ms [%s] %s\n", ts, (unsigned)ms, slot->tag, slot->msg);
                sd_io_end();
                s_pending_lines++;
                sd_maybe_flush(ms);
            }
        }
    }
    log_unlock();
}

void log_dump_recent(const char *tag_filter, log_level_t min_level, size_t limit) {
    if (!log_lock()) {
        return;
    }
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
    log_unlock();
}

void log_sd_set_enabled(bool enabled) {
    if (!log_lock()) {
        return;
    }
    s_sd_enabled = enabled;
    if (!enabled && s_log_file) {
        sd_io_begin();
        fflush(s_log_file);
        fclose(s_log_file);
        sd_io_end();
        s_log_file = NULL;
    }
    log_unlock();
}

void log_sd_force_rotate(void) {
    if (!log_lock()) {
        return;
    }
    if (s_log_file) {
        sd_io_begin();
        fflush(s_log_file);
        fclose(s_log_file);
        sd_io_end();
        s_log_file = NULL;
    }
    s_last_log_date = -1;
    s_log_file_attempted = false;
    log_unlock();
}

void log_sd_notify_mounted(void) {
    if (!log_lock()) {
        return;
    }
    // Reset retry bookkeeping and try immediately.
    s_log_file_attempted = false;
    s_logged_open_success = false;
    s_logged_open_fail = false;
    s_last_retry_ms = 0;
    s_last_log_date = -1;
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    sd_try_open(ms);
    log_unlock();
}

void log_set_console_level(log_level_t level) {
    if (!log_lock()) {
        return;
    }
    s_console_min_level = level;
    log_unlock();
}

void log_set_console_level_temporary(log_level_t level, uint32_t duration_ms, log_level_t revert_level) {
    if (!log_lock()) {
        return;
    }
    s_console_min_level = level;
    s_console_level_target = revert_level;
    s_console_level_decay_ms = (duration_ms > 0) ? (uint32_t)(esp_timer_get_time() / 1000ULL) + duration_ms : 0;
    log_unlock();
}

bool log_console_set_tag_rule(const char *tag, bool enabled, log_level_t min_level)
{
    if (!tag || !tag[0]) {
        return false;
    }
    if (min_level < LOG_LEVEL_DEBUG || min_level > LOG_LEVEL_ERROR) {
        min_level = LOG_LEVEL_INFO;
    }
    if (!log_lock()) {
        return false;
    }

    int idx = find_console_tag_rule(tag);
    if (idx < 0) {
        for (int i = 0; i < CONSOLE_TAG_RULE_MAX; ++i) {
            if (!s_console_tag_rules[i].used) {
                idx = i;
                break;
            }
        }
    }

    if (idx < 0) {
        log_unlock();
        return false;
    }

    s_console_tag_rules[idx].used = true;
    s_console_tag_rules[idx].enabled = enabled;
    s_console_tag_rules[idx].min_level = min_level;
    strlcpy(s_console_tag_rules[idx].tag, tag, sizeof(s_console_tag_rules[idx].tag));
    log_unlock();
    return true;
}

void log_console_clear_tag_rule(const char *tag)
{
    if (!tag || !tag[0]) {
        return;
    }
    if (!log_lock()) {
        return;
    }
    int idx = find_console_tag_rule(tag);
    if (idx >= 0) {
        memset(&s_console_tag_rules[idx], 0, sizeof(s_console_tag_rules[idx]));
    }
    log_unlock();
}

void log_console_clear_all_tag_rules(void)
{
    if (!log_lock()) {
        return;
    }
    memset(s_console_tag_rules, 0, sizeof(s_console_tag_rules));
    log_unlock();
}

static int cmp_tag_info(const void *a, const void *b)
{
    const log_tag_info_t *lhs = (const log_tag_info_t *)a;
    const log_tag_info_t *rhs = (const log_tag_info_t *)b;
    return strcasecmp(lhs->tag, rhs->tag);
}

size_t log_get_tag_snapshot(log_tag_info_t *out, size_t max_entries, size_t *out_total)
{
    size_t total = 0;
    size_t written = 0;
    if (!log_lock()) {
        if (out_total) {
            *out_total = 0;
        }
        return 0;
    }

    for (int i = 0; i < KNOWN_TAG_MAX; ++i) {
        if (!s_known_tags[i].used) {
            continue;
        }
        total++;
        if (!out || written >= max_entries) {
            continue;
        }

        log_tag_info_t *dst = &out[written++];
        memset(dst, 0, sizeof(*dst));
        strlcpy(dst->tag, s_known_tags[i].tag, sizeof(dst->tag));
        dst->seen_count = s_known_tags[i].seen_count;
        dst->last_seen_ms = (uint32_t)(s_known_tags[i].last_seen_us / 1000ULL);

        int ridx = find_console_tag_rule(s_known_tags[i].tag);
        dst->has_console_rule = (ridx >= 0);
        if (ridx >= 0) {
            dst->console_enabled = s_console_tag_rules[ridx].enabled;
            dst->console_min_level = s_console_tag_rules[ridx].min_level;
        } else {
            dst->console_enabled = !console_mute_tag(s_known_tags[i].tag);
            dst->console_min_level = s_console_min_level;
        }
    }

    if (written > 1) {
        qsort(out, written, sizeof(out[0]), cmp_tag_info);
    }

    if (out_total) {
        *out_total = total;
    }
    log_unlock();
    return written;
}

void log_task_start(void) {
    // Task is created in log_init if missing; this function can be used to ensure start.
    if (s_log_task_handle == NULL) {
        log_init();
    }
}

void log_set_prompt(const char *prompt) {
    if (!log_lock()) {
        return;
    }
    if (prompt && prompt[0]) {
        strlcpy(s_prompt, prompt, sizeof(s_prompt));
        s_prompt_enabled = true;
    } else {
        s_prompt_enabled = false;
        s_prompt[0] = '\0';
    }
    log_unlock();
}

static void log_background_task(void *param) {
    (void)param;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!log_lock()) {
            continue;
        }
        uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (s_console_level_decay_ms > 0 && ms >= s_console_level_decay_ms) {
            s_console_min_level = s_console_level_target;
            s_console_level_decay_ms = 0;
        }
        sd_try_open(ms);
        if (s_log_file) {
            rotate_log_if_needed(ms);
            sd_maybe_flush(ms);
        }
        log_unlock();
    }
}
