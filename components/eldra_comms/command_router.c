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
    datetime_to_str(ts, dt);
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
    datetime_to_str(ts, current);
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
    datetime_to_str(ts, current);
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
