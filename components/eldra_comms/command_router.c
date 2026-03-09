#include "eldra_comms.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "eldra_emotion.h"
#include "eldra_logging.h"
#include "eldra_sensors.h"

#define MAX_COMMANDS 16
#define LINE_BUF_MAX 128

typedef struct {
    const char *name;
    const char *help;
    console_cmd_handler_t handler;
} command_entry_t;

static command_entry_t s_commands[MAX_COMMANDS];
static size_t s_command_count = 0;
static emotion_context_t *s_emotion_ctx = NULL;
static const char *TAG = "cmd_router";

static void trim(char *s) {
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || isspace((unsigned char)s[len - 1]))) {
        s[--len] = '\0';
    }
    while (*s && isspace((unsigned char)*s)) {
        memmove(s, s + 1, strlen(s));
    }
}

static bool parse_uint(const char *s, uint32_t *out) {
    if (!s || !out) {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static log_level_t parse_level(const char *s, bool *matched) {
    if (matched) {
        *matched = true;
    }
    if (strcasecmp(s, "DEBUG") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(s, "INFO") == 0) return LOG_LEVEL_INFO;
    if (strcasecmp(s, "WARN") == 0) return LOG_LEVEL_WARN;
    if (strcasecmp(s, "ERROR") == 0) return LOG_LEVEL_ERROR;
    if (matched) {
        *matched = false;
    }
    return LOG_LEVEL_DEBUG;
}

static bool cmd_logs(const char *args) {
    char buf[LINE_BUF_MAX];
    if (args) {
        strncpy(buf, args, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    } else {
        buf[0] = '\0';
    }
    const char *tag = NULL;
    log_level_t level = LOG_LEVEL_DEBUG;
    size_t limit = 100;
    if (buf[0] != '\0') {
        char *token = strtok(buf, " ");
        while (token) {
            bool is_level = false;
            log_level_t lvl = parse_level(token, &is_level);
            if (is_level) {
                level = lvl;
            } else {
                uint32_t tmp = 0;
                if (parse_uint(token, &tmp)) {
                    limit = tmp;
                } else if (!tag) {
                    tag = token;
                }
            }
            token = strtok(NULL, " ");
        }
    }
    log_event(LOG_LEVEL_INFO, TAG, "LOGS tag=%s level>=%d limit=%u", tag ? tag : "any", (int)level,
              (unsigned)limit);
    log_dump_recent(tag, level, limit);
    return true;
}

static void format_datetime_str(char *buf, size_t len, const datetime_t *dt)
{
    if (!buf || !dt) {
        return;
    }
    snprintf(buf, len, "%02u/%02u/%04u %02u:%02u:%02u",
             (unsigned)dt->day, (unsigned)dt->month, (unsigned)dt->year,
             (unsigned)dt->hour, (unsigned)dt->minute, (unsigned)dt->second);
}

static bool cmd_settime(const char *args) {
    if (!args || !*args) {
        log_event(LOG_LEVEL_WARN, TAG, "SETTIME requires epoch ms/seconds or dd/mm/yyyy-HH:MM:SS");
        return true;
    }
    datetime_t dt = {0};
    bool parsed = false;
    // Try human format dd/mm/yyyy-HH:MM:SS
    if (strchr(args, '/') && strchr(args, '-')) {
        unsigned d = 0, m = 0, y = 0, hh = 0, mm = 0, ss = 0;
        int res = sscanf(args, "%u/%u/%u-%u:%u:%u", &d, &m, &y, &hh, &mm, &ss);
        if (res == 6 && d <= 31 && m >= 1 && m <= 12) {
            dt.day = (uint8_t)d;
            dt.month = (uint8_t)m;
            dt.year = (uint16_t)y;
            dt.hour = (uint8_t)hh;
            dt.minute = (uint8_t)mm;
            dt.second = (uint8_t)ss;
            // dotw left as 0; RTC will derive or can ignore.
            parsed = true;
        }
    }
    if (!parsed) {
        uint64_t value = strtoull(args, NULL, 10);
        if (value == 0) {
            log_event(LOG_LEVEL_WARN, TAG, "SETTIME parse failed");
            return true;
        }
        if (value < 100000000000ULL) {
            value *= 1000ULL;
        }
        time_t sec = (time_t)(value / 1000ULL);
        struct tm tm_out;
        gmtime_r(&sec, &tm_out);
        dt.year = (uint16_t)(tm_out.tm_year + 1900);
        dt.month = (uint8_t)(tm_out.tm_mon + 1);
        dt.day = (uint8_t)tm_out.tm_mday;
        dt.dotw = (uint8_t)tm_out.tm_wday;
        dt.hour = (uint8_t)tm_out.tm_hour;
        dt.minute = (uint8_t)tm_out.tm_min;
        dt.second = (uint8_t)tm_out.tm_sec;
    }
    if (eldra_sensors_rtc_set(&dt) != ESP_OK) {
        log_event(LOG_LEVEL_WARN, TAG, "SETTIME failed");
        return true;
    }
    char ts[64] = {0};
    format_datetime_str(ts, sizeof(ts), &dt);
    log_event(LOG_LEVEL_INFO, TAG, "RTC set to %s", ts);
    return true;
}

static bool cmd_setdate(const char *args) {
    if (!args || !*args) {
        log_event(LOG_LEVEL_WARN, TAG, "SETDATE requires dd/mm/yyyy");
        return true;
    }
    unsigned d = 0, m = 0, y = 0;
    int res = sscanf(args, "%u/%u/%u", &d, &m, &y);
    if (res != 3 || d == 0 || d > 31 || m == 0 || m > 12) {
        log_event(LOG_LEVEL_WARN, TAG, "SETDATE parse failed");
        return true;
    }
    datetime_t current = {0};
    eldra_sensors_rtc_get(&current);
    current.day = (uint8_t)d;
    current.month = (uint8_t)m;
    current.year = (uint16_t)y;
    if (eldra_sensors_rtc_set(&current) != ESP_OK) {
        log_event(LOG_LEVEL_WARN, TAG, "SETDATE failed");
        return true;
    }
    char ts[64] = {0};
    format_datetime_str(ts, sizeof(ts), &current);
    log_event(LOG_LEVEL_INFO, TAG, "RTC date set to %s", ts);
    return true;
}

static bool cmd_setclock(const char *args) {
    if (!args || !*args) {
        log_event(LOG_LEVEL_WARN, TAG, "SETCLOCK requires HH:MM:SS");
        return true;
    }
    unsigned hh = 0, mm = 0, ss = 0;
    int res = sscanf(args, "%u:%u:%u", &hh, &mm, &ss);
    if (res != 3 || hh > 23 || mm > 59 || ss > 59) {
        log_event(LOG_LEVEL_WARN, TAG, "SETCLOCK parse failed");
        return true;
    }
    datetime_t current = {0};
    eldra_sensors_rtc_get(&current);
    current.hour = (uint8_t)hh;
    current.minute = (uint8_t)mm;
    current.second = (uint8_t)ss;
    if (eldra_sensors_rtc_set(&current) != ESP_OK) {
        log_event(LOG_LEVEL_WARN, TAG, "SETCLOCK failed");
        return true;
    }
    char ts[64] = {0};
    format_datetime_str(ts, sizeof(ts), &current);
    log_event(LOG_LEVEL_INFO, TAG, "RTC time set to %s", ts);
    return true;
}

static bool cmd_bat(const char *args) {
    (void)args;
    log_event(LOG_LEVEL_INFO, TAG, "BAT %.2fV (~%u%%)", eldra_sensors_get_battery_voltage(),
              (unsigned)eldra_sensors_get_battery_percent());
    return true;
}

static bool cmd_state(const char *args) {
    (void)args;
    if (!s_emotion_ctx) {
        log_event(LOG_LEVEL_INFO, TAG, "STATE: emotion context not available");
        return true;
    }
    emotion_context_t *ctx = s_emotion_ctx;
    log_event(LOG_LEVEL_INFO, TAG,
              "STATE %d prev=%d happy=%u hunger=%u energy=%u social=%u fear=%u eldritch=%u batt=%u%%",
              (int)ctx->current_state, (int)ctx->previous_state, ctx->happiness, ctx->hunger, ctx->energy,
              ctx->social, ctx->fear, ctx->eldritch_charge, ctx->battery_percent);
    return true;
}

static bool cmd_feed(const char *args) {
    pet_command_t pcmd = {.type = CMD_FEED, .arg0 = args ? (uint32_t)strtoul(args, NULL, 10) : 0};
    comms_enqueue_command(&pcmd);
    return true;
}

static bool cmd_pet(const char *args) {
    (void)args;
    pet_command_t pcmd = {.type = CMD_PET};
    comms_enqueue_command(&pcmd);
    return true;
}

static bool cmd_play(const char *args) {
    (void)args;
    pet_command_t pcmd = {.type = CMD_PLAY};
    comms_enqueue_command(&pcmd);
    return true;
}

static bool cmd_forcestate(const char *args) {
    pet_command_t pcmd = {.type = CMD_DEBUG_FORCE_STATE, .arg0 = args ? (uint32_t)strtoul(args, NULL, 10) : 0};
    comms_enqueue_command(&pcmd);
    return true;
}

static bool cmd_logsd(const char *args) {
    if (!args || !*args) {
        log_event(LOG_LEVEL_INFO, TAG, "LOGSD requires ON/OFF/ROTATE");
        return true;
    }
    if (strcasecmp(args, "ON") == 0) {
        log_sd_set_enabled(true);
        log_event(LOG_LEVEL_INFO, TAG, "SD logging enabled");
    } else if (strcasecmp(args, "OFF") == 0) {
        log_sd_set_enabled(false);
        log_event(LOG_LEVEL_INFO, TAG, "SD logging disabled");
    } else if (strcasecmp(args, "ROTATE") == 0) {
        log_sd_force_rotate();
        log_event(LOG_LEVEL_INFO, TAG, "SD log rotation requested");
    } else {
        log_event(LOG_LEVEL_WARN, TAG, "LOGSD unknown arg (use ON/OFF/ROTATE)");
    }
    return true;
}

static bool cmd_loglevel(const char *args) {
    if (!args || !*args) {
        log_event(LOG_LEVEL_INFO, TAG, "LOGLEVEL requires DEBUG/INFO/WARN/ERROR");
        return true;
    }
    bool matched = false;
    log_level_t lvl = parse_level(args, &matched);
    if (!matched) {
        log_event(LOG_LEVEL_WARN, TAG, "LOGLEVEL unknown arg");
        return true;
    }
    log_set_console_level(lvl);
    log_event(LOG_LEVEL_INFO, TAG, "Console log level set to %s", args);
    return true;
}

static bool cmd_logtag(const char *args) {
    if (!args || !*args) {
        log_event(LOG_LEVEL_INFO, TAG, "LOGTAG usage: LOGTAG <tag> ON|OFF [DEBUG|INFO|WARN|ERROR]");
        return true;
    }

    char buf[LINE_BUF_MAX];
    strlcpy(buf, args, sizeof(buf));
    char *tag = strtok(buf, " ");
    char *mode = strtok(NULL, " ");
    char *lvl_s = strtok(NULL, " ");
    if (!tag || !mode) {
        log_event(LOG_LEVEL_WARN, TAG, "LOGTAG missing args");
        return true;
    }

    bool enable = false;
    if (strcasecmp(mode, "ON") == 0) {
        enable = true;
    } else if (strcasecmp(mode, "OFF") == 0) {
        enable = false;
    } else {
        log_event(LOG_LEVEL_WARN, TAG, "LOGTAG mode must be ON/OFF");
        return true;
    }

    log_level_t lvl = LOG_LEVEL_INFO;
    if (lvl_s && *lvl_s) {
        bool matched = false;
        lvl = parse_level(lvl_s, &matched);
        if (!matched) {
            log_event(LOG_LEVEL_WARN, TAG, "LOGTAG level invalid");
            return true;
        }
    }

    if (!log_console_set_tag_rule(tag, enable, lvl)) {
        log_event(LOG_LEVEL_WARN, TAG, "LOGTAG failed (table full?) tag=%s", tag);
        return true;
    }

    log_event(LOG_LEVEL_INFO, TAG, "LOGTAG tag=%s mode=%s level>=%d", tag, enable ? "ON" : "OFF", (int)lvl);
    return true;
}

static bool cmd_logtagclr(const char *args) {
    if (!args || !*args) {
        log_console_clear_all_tag_rules();
        log_event(LOG_LEVEL_INFO, TAG, "LOGTAGCLR all rules cleared");
        return true;
    }
    log_console_clear_tag_rule(args);
    log_event(LOG_LEVEL_INFO, TAG, "LOGTAGCLR tag=%s", args);
    return true;
}

static bool cmd_logtags(const char *args)
{
    size_t limit = 64;
    if (args && *args) {
        uint32_t parsed = 0;
        if (parse_uint(args, &parsed) && parsed > 0) {
            limit = parsed;
        }
    }
    if (limit > 64) {
        limit = 64;
    }

    log_tag_info_t info[64];
    size_t total = 0;
    size_t shown = log_get_tag_snapshot(info, limit, &total);
    log_event(LOG_LEVEL_INFO, TAG, "LOGTAGS total=%u showing=%u", (unsigned)total, (unsigned)shown);
    for (size_t i = 0; i < shown; ++i) {
        log_event(LOG_LEVEL_INFO, TAG, "  %-16s seen=%u last=%ums console=%s >=%d (%s)",
                  info[i].tag,
                  (unsigned)info[i].seen_count,
                  (unsigned)info[i].last_seen_ms,
                  info[i].console_enabled ? "ON" : "OFF",
                  (int)info[i].console_min_level,
                  info[i].has_console_rule ? "rule" : "default");
    }
    return true;
}

typedef struct {
    const char *name;
    const char *const *tags;
    size_t tag_count;
} log_focus_group_t;

static const char *const k_focus_scaffold[] = {
    "app_main", "cmd_router", "comms", "console", "jobs",
    "console_sd", "console_wifi", "console_rtc", "console_imu", "console_emotion"
};

static const char *const k_focus_display[] = {
    "eldra_display_round", "LCD", "eldra_eyes", "eldra_glyphs", "backlight", "exio"
};

static const char *const k_focus_sleep[] = {
    "eldra_sleep", "emotion", "eldra_eyes"
};

static const char *const k_focus_cloud[] = {
    "eldra_cloud", "cmd_router", "console_sd"
};

static const char *const k_focus_wifi[] = {
    "wifi_drv", "console_wifi", "eldra_cloud", "app_main"
};

static const char *const k_focus_sensors[] = {
    "eldra_sensors", "qmi8658", "ADC", "I2C", "console_imu"
};

static const char *const k_focus_storage[] = {
    "sd_drv", "config_store", "eldra_logging", "rtc_drv", "console_sd"
};

static const log_focus_group_t k_log_focus_groups[] = {
    {.name = "scaffold", .tags = k_focus_scaffold, .tag_count = sizeof(k_focus_scaffold) / sizeof(k_focus_scaffold[0])},
    {.name = "display",  .tags = k_focus_display,  .tag_count = sizeof(k_focus_display) / sizeof(k_focus_display[0])},
    {.name = "sleep",    .tags = k_focus_sleep,    .tag_count = sizeof(k_focus_sleep) / sizeof(k_focus_sleep[0])},
    {.name = "cloud",    .tags = k_focus_cloud,    .tag_count = sizeof(k_focus_cloud) / sizeof(k_focus_cloud[0])},
    {.name = "wifi",     .tags = k_focus_wifi,     .tag_count = sizeof(k_focus_wifi) / sizeof(k_focus_wifi[0])},
    {.name = "sensors",  .tags = k_focus_sensors,  .tag_count = sizeof(k_focus_sensors) / sizeof(k_focus_sensors[0])},
    {.name = "storage",  .tags = k_focus_storage,  .tag_count = sizeof(k_focus_storage) / sizeof(k_focus_storage[0])},
};

static bool cmd_logfocus(const char *args)
{
    if (!args || !*args) {
        log_event(LOG_LEVEL_INFO, TAG,
                  "LOGFOCUS usage: LOGFOCUS OFF|SCAFFOLD|DISPLAY|SLEEP|CLOUD|WIFI|SENSORS|STORAGE");
        return true;
    }

    if (strcasecmp(args, "OFF") == 0 || strcasecmp(args, "ALL") == 0) {
        log_console_clear_all_tag_rules();
        log_set_console_level(LOG_LEVEL_INFO);
        log_event(LOG_LEVEL_INFO, TAG, "LOGFOCUS disabled");
        return true;
    }

    const log_focus_group_t *group = NULL;
    for (size_t i = 0; i < sizeof(k_log_focus_groups) / sizeof(k_log_focus_groups[0]); ++i) {
        if (strcasecmp(args, k_log_focus_groups[i].name) == 0) {
            group = &k_log_focus_groups[i];
            break;
        }
    }
    if (!group) {
        log_event(LOG_LEVEL_WARN, TAG, "LOGFOCUS unknown group=%s", args);
        return true;
    }

    log_console_clear_all_tag_rules();
    log_set_console_level(LOG_LEVEL_WARN);
    bool ok = true;
    ok &= log_console_set_tag_rule("console_sd", true, LOG_LEVEL_INFO);
    ok &= log_console_set_tag_rule("cmd_router", true, LOG_LEVEL_INFO);
    for (size_t i = 0; i < group->tag_count; ++i) {
        ok &= log_console_set_tag_rule(group->tags[i], true, LOG_LEVEL_DEBUG);
    }
    log_event(LOG_LEVEL_INFO, TAG, "LOGFOCUS %s %s", group->name, ok ? "OK" : "PARTIAL");
    return true;
}

static bool cmd_bridgestats(const char *args)
{
    (void)args;
    comms_stats_t st = {0};
    comms_get_stats(&st);
    log_event(LOG_LEVEL_INFO, TAG,
              "BRIDGESTATS depth=%u max=%u cap=16 enq=%u drop_full=%u drop_lock=%u deq=%u lat_ms(last=%u avg=%u max=%u)",
              (unsigned)st.queue_depth,
              (unsigned)st.queue_max_depth,
              (unsigned)st.enqueue_ok,
              (unsigned)st.enqueue_drop_full,
              (unsigned)st.enqueue_drop_lock,
              (unsigned)st.dequeue_ok,
              (unsigned)st.dequeue_latency_last_ms,
              (unsigned)st.dequeue_latency_avg_ms,
              (unsigned)st.dequeue_latency_max_ms);
    return true;
}

bool comms_commands_register(const char *name, console_cmd_handler_t handler, const char *help) {
    if (!name || !handler || s_command_count >= MAX_COMMANDS) {
        return false;
    }
    s_commands[s_command_count].name = name;
    s_commands[s_command_count].handler = handler;
    s_commands[s_command_count].help = help;
    s_command_count++;
    return true;
}

void comms_commands_init(emotion_context_t *ctx) {
    s_emotion_ctx = ctx;
    s_command_count = 0;
    comms_commands_register("LOGS", cmd_logs, "LOGS [tag] [level] [limit]");
    comms_commands_register("SETTIME", cmd_settime, "SETTIME <epoch_ms|dd/mm/yyyy-HH:MM:SS>");
    comms_commands_register("SETDATE", cmd_setdate, "SETDATE dd/mm/yyyy");
    comms_commands_register("SETCLOCK", cmd_setclock, "SETCLOCK HH:MM:SS");
    comms_commands_register("BAT", cmd_bat, "Show battery voltage/percent");
    comms_commands_register("STATE", cmd_state, "Show emotion state/meters");
    comms_commands_register("FEED", cmd_feed, "FEED [type]");
    comms_commands_register("PET", cmd_pet, "PET");
    comms_commands_register("PLAY", cmd_play, "PLAY");
    comms_commands_register("FORCESTATE", cmd_forcestate, "FORCESTATE <state_id>");
    comms_commands_register("LOGSD", cmd_logsd, "LOGSD ON|OFF|ROTATE");
    comms_commands_register("LOGLEVEL", cmd_loglevel, "LOGLEVEL DEBUG|INFO|WARN|ERROR");
    comms_commands_register("LOGTAG", cmd_logtag, "LOGTAG <tag> ON|OFF [DEBUG|INFO|WARN|ERROR]");
    comms_commands_register("LOGTAGCLR", cmd_logtagclr, "LOGTAGCLR [tag]");
    comms_commands_register("LOGTAGS", cmd_logtags, "LOGTAGS [limit]");
    comms_commands_register("LOGFOCUS", cmd_logfocus, "LOGFOCUS OFF|SCAFFOLD|DISPLAY|SLEEP|CLOUD|WIFI|SENSORS|STORAGE");
    comms_commands_register("BRIDGESTATS", cmd_bridgestats, "BRIDGESTATS");
}

void comms_commands_process_line(const char *line) {
    if (!line) {
        return;
    }
    char buf[LINE_BUF_MAX];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trim(buf);
    if (buf[0] == '\0') {
        return;
    }
    char *cmd = strtok(buf, " ");
    char *args = strtok(NULL, "");
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < s_command_count; ++i) {
        if (strcasecmp(cmd, s_commands[i].name) == 0 && s_commands[i].handler) {
            s_commands[i].handler(args);
            return;
        }
    }
    log_event(LOG_LEVEL_WARN, TAG, "Unknown command: %s", cmd);
}
#include "eldra_comms.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "eldra_emotion.h"
#include "eldra_logging.h"
#include "eldra_sensors.h"

/**
 * @file command_router.c
 * @brief Shared command parser/dispatcher for UART console and future HTTP endpoints.
 */
