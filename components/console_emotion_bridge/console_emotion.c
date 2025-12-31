#include "console_emotion.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_console.h"
#include "eldra_comms.h"
#include "eldra_eyes.h"
#include "config_store.h"

static const char *TAG = "console_emotion";
static emotion_context_t *s_ctx = NULL;

static const char *state_name(emotion_state_t st)
{
    switch (st) {
        case EMOTION_STATE_NEUTRAL: return "NEUTRAL";
        case EMOTION_STATE_HAPPY: return "HAPPY";
        case EMOTION_STATE_SAD: return "SAD";
        case EMOTION_STATE_LONELY: return "LONELY";
        case EMOTION_STATE_SLEEPY: return "SLEEPY";
        case EMOTION_STATE_HUNGRY: return "HUNGRY";
        case EMOTION_STATE_PLAYFUL: return "PLAYFUL";
        case EMOTION_STATE_ELDRITCH: return "ELDRITCH";
        case EMOTION_STATE_SCARED: return "SCARED";
        case EMOTION_STATE_DIZZY: return "DIZZY";
        default: return "UNKNOWN";
    }
}

static emotion_state_t parse_state(const char *s, bool *ok)
{
    if (ok) *ok = true;
    if (!s) {
        if (ok) *ok = false;
        return EMOTION_STATE_NEUTRAL;
    }
    if (strcasecmp(s, "NEUTRAL") == 0) return EMOTION_STATE_NEUTRAL;
    if (strcasecmp(s, "HAPPY") == 0) return EMOTION_STATE_HAPPY;
    if (strcasecmp(s, "SAD") == 0) return EMOTION_STATE_SAD;
    if (strcasecmp(s, "LONELY") == 0) return EMOTION_STATE_LONELY;
    if (strcasecmp(s, "SLEEPY") == 0) return EMOTION_STATE_SLEEPY;
    if (strcasecmp(s, "HUNGRY") == 0) return EMOTION_STATE_HUNGRY;
    if (strcasecmp(s, "PLAYFUL") == 0) return EMOTION_STATE_PLAYFUL;
    if (strcasecmp(s, "ELDRITCH") == 0) return EMOTION_STATE_ELDRITCH;
    if (strcasecmp(s, "SCARED") == 0) return EMOTION_STATE_SCARED;
    if (strcasecmp(s, "DIZZY") == 0) return EMOTION_STATE_DIZZY;

    // Accept numeric enums for convenience.
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end != s && v >= EMOTION_STATE_NEUTRAL && v <= EMOTION_STATE_DIZZY) {
        return (emotion_state_t)v;
    }
    if (ok) *ok = false;
    return EMOTION_STATE_NEUTRAL;
}

static int cmd_feed(int argc, char **argv)
{
    uint32_t food = 0;
    if (argc >= 2) {
        food = (uint32_t)strtoul(argv[1], NULL, 10);
    }
    pet_command_t cmd = {.type = CMD_FEED, .arg0 = food};
    if (!comms_enqueue_command(&cmd)) {
        printf("Queue full\n");
    } else {
        printf("Queued FEED (%lu)\n", (unsigned long)food);
    }
    return 0;
}

static int cmd_pet(int argc, char **argv)
{
    (void)argc; (void)argv;
    pet_command_t cmd = {.type = CMD_PET};
    if (!comms_enqueue_command(&cmd)) {
        printf("Queue full\n");
    } else {
        printf("Queued PET\n");
    }
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    (void)argc; (void)argv;
    pet_command_t cmd = {.type = CMD_PLAY};
    if (!comms_enqueue_command(&cmd)) {
        printf("Queue full\n");
    } else {
        printf("Queued PLAY\n");
    }
    return 0;
}

static int cmd_state(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: emo_state <name|id>\n");
        return 0;
    }
    bool ok = false;
    emotion_state_t st = parse_state(argv[1], &ok);
    if (!ok) {
        printf("Unknown state '%s'\n", argv[1]);
        return 0;
    }
    pet_command_t cmd = {.type = CMD_DEBUG_FORCE_STATE, .arg0 = (uint32_t)st};
    if (!comms_enqueue_command(&cmd)) {
        printf("Queue full\n");
    } else {
        printf("Queued STATE %s (%d)\n", state_name(st), (int)st);
    }
    return 0;
}

static int cmd_state_show(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!s_ctx) {
        printf("Emotion context not available\n");
        return 0;
    }
    printf("Emotion state: %s (%d)\n", state_name(s_ctx->current_state), (int)s_ctx->current_state);
    printf("Meters: happy=%u hunger=%u energy=%u social=%u fear=%u eldritch=%u batt=%u%%\n",
           s_ctx->happiness, s_ctx->hunger, s_ctx->energy, s_ctx->social,
           s_ctx->fear, s_ctx->eldritch_charge, s_ctx->battery_percent);
    return 0;
}

static int cmd_eyes_offset(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: eyes_offset <x_px> <y_px> [persist]\n");
        return 0;
    }
    int x = (int)strtol(argv[1], NULL, 10);
    int y = (int)strtol(argv[2], NULL, 10);
    bool persist = (argc >= 4 && strcasecmp(argv[3], "persist") == 0);
    eldra_eyes_set_center_offset(x, y);
    if (persist) {
        config_store_t cfg;
        if (config_store_load(&cfg) == ESP_OK) {
            cfg.eyes_center_x_offset = x;
            cfg.eyes_center_y_offset = y;
            if (config_store_save(&cfg) == ESP_OK) {
                printf("Eyes center offset persisted to config (x=%d y=%d)\n", x, y);
            } else {
                printf("Persist failed (save error)\n");
            }
        } else {
            printf("Persist failed (config load error)\n");
        }
    } else {
        printf("Eyes center offset set to x=%d y=%d (not persisted)\n", x, y);
    }
    return 0;
}

esp_err_t ConsoleEmotion_Init(emotion_context_t *ctx)
{
    s_ctx = ctx;

    const esp_console_cmd_t feed_cmd = {
        .command = "emo_feed",
        .help = "Feed Eldra. Usage: emo_feed [food_id]",
        .hint = NULL,
        .func = &cmd_feed,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&feed_cmd), TAG, "register emo_feed failed");

    const esp_console_cmd_t pet_cmd = {
        .command = "emo_pet",
        .help = "Pet Eldra.",
        .hint = NULL,
        .func = &cmd_pet,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&pet_cmd), TAG, "register emo_pet failed");

    const esp_console_cmd_t play_cmd = {
        .command = "emo_play",
        .help = "Play with Eldra.",
        .hint = NULL,
        .func = &cmd_play,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&play_cmd), TAG, "register emo_play failed");

    const esp_console_cmd_t state_cmd = {
        .command = "emo_state",
        .help = "Force emotion state. Usage: emo_state <name|id>",
        .hint = NULL,
        .func = &cmd_state,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&state_cmd), TAG, "register emo_state failed");

    const esp_console_cmd_t show_cmd = {
        .command = "emo_state_show",
        .help = "Show current emotion state/meters.",
        .hint = NULL,
        .func = &cmd_state_show,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&show_cmd), TAG, "register emo_state_show failed");

    const esp_console_cmd_t offset_cmd = {
        .command = "eyes_offset",
        .help = "Set eye center offset in pixels. Usage: eyes_offset <x> <y> (positive=right/down)",
        .hint = NULL,
        .func = &cmd_eyes_offset,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&offset_cmd), TAG, "register eyes_offset failed");

    return ESP_OK;
}
