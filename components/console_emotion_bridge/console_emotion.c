#include "console_emotion.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_console.h"
#include "eldra_comms.h"
#include "eldra_eyes.h"
#include "eldra_glyphs.h"
#include "config_store.h"

static const char *TAG = "console_emotion";
static emotion_context_t *s_ctx = NULL;

void eldra_request_render_now(void) __attribute__((weak));
void eldra_set_sleep_eye_override(uint8_t mode) __attribute__((weak));
uint8_t eldra_get_sleep_eye_override(void) __attribute__((weak));

static int clamp_meter(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int clamp_weight_pct(int v)
{
    if (v < 0) return 0;
    if (v > 300) return 300;
    return v;
}

static void print_affect_weights(void)
{
    emotion_affect_weights_t w = {0};
    emotion_get_affect_weights(&w);
    printf("Affect weights (pct): happy=%u satiety=%u energy=%u social=%u fear=%u\n",
           (unsigned)w.happiness_pct,
           (unsigned)w.satiety_pct,
           (unsigned)w.energy_pct,
           (unsigned)w.social_pct,
           (unsigned)w.fear_pct);
}

static const char *state_name(emotion_state_t st)
{
    switch (st) {
        case EMOTION_STATE_NEUTRAL: return "NEUTRAL";
        case EMOTION_STATE_HAPPY: return "HAPPY";
        case EMOTION_STATE_SAD: return "SAD";
        case EMOTION_STATE_ANGRY: return "ANGRY";
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
    if (strcasecmp(s, "ANGRY") == 0) return EMOTION_STATE_ANGRY;
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
    if (end != s && v >= EMOTION_STATE_NEUTRAL && v <= EMOTION_STATE_ANGRY) {
        return (emotion_state_t)v;
    }
    if (ok) *ok = false;
    return EMOTION_STATE_NEUTRAL;
}

static int cmd_feed(int argc, char **argv)
{
    uint32_t food = 0; // 0=big, 1=small, 2=snack
    if (argc >= 2) {
        food = (uint32_t)strtoul(argv[1], NULL, 10);
    }
    pet_command_t cmd = {.type = CMD_FEED, .arg0 = food};
    if (!comms_enqueue_command(&cmd)) {
        printf("Queue full\n");
    } else {
        const char *label = (food == 0) ? "BIG" : (food == 1) ? "SMALL" : "SNACK";
        printf("Queued FEED %s (%lu)\n", label, (unsigned long)food);
    }
    return 0;
}

static int cmd_pet(int argc, char **argv)
{
    uint32_t size = 0; // 0=small,1=big
    if (argc >= 2) {
        if (strcasecmp(argv[1], "big") == 0 || strcasecmp(argv[1], "large") == 0) {
            size = 1;
        } else {
            size = (uint32_t)strtoul(argv[1], NULL, 10);
            if (size > 1) size = 1;
        }
    }
    pet_command_t cmd = {.type = CMD_PET, .arg0 = size};
    if (!comms_enqueue_command(&cmd)) {
        printf("Queue full\n");
    } else {
        printf("Queued PET (%s)\n", size ? "big" : "small");
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
    printf("Meters: happy=%u hunger(satiety)=%u energy=%u social=%u fear=%u eldritch=%u batt=%u%%\n",
           s_ctx->happiness, s_ctx->hunger, s_ctx->energy, s_ctx->social,
           s_ctx->fear, s_ctx->eldritch_charge, s_ctx->battery_percent);
    emotion_need_mask_t needs = emotion_get_needs(s_ctx);
    int8_t val = 0, aro = 0;
    emotion_affect_t aff = emotion_get_affect(s_ctx, &val, &aro);
    const char *aff_name = "NEUTRAL";
    switch (aff) {
        case EMO_AFFECT_HAPPY: aff_name = "HAPPY"; break;
        case EMO_AFFECT_SAD: aff_name = "SAD"; break;
        case EMO_AFFECT_ANGRY: aff_name = "ANGRY"; break;
        case EMO_AFFECT_EXCITED: aff_name = "EXCITED"; break;
        default: break;
    }
    printf("Needs mask: 0x%02X (HUNGRY=%d LONELY=%d SLEEPY=%d SCARED=%d PLAYFUL=%d ELDRITCH_READY=%d)\n",
           needs,
           !!(needs & EMO_NEED_HUNGRY),
           !!(needs & EMO_NEED_LONELY),
           !!(needs & EMO_NEED_SLEEPY),
           !!(needs & EMO_NEED_SCARED),
           !!(needs & EMO_NEED_PLAYFUL),
           !!(needs & EMO_NEED_ELDRITCH_READY));
    printf("Affect: %s valence=%d arousal=%d\n", aff_name, (int)val, (int)aro);
    return 0;
}

static int cmd_log_interval(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: emo_log_interval <minutes> (0=off)\n");
        return 0;
    }
    int minutes = atoi(argv[1]);
    if (minutes < 0) minutes = 0;
    emotion_set_mood_log_interval_minutes((uint32_t)minutes);
    config_store_t cfg;
    if (config_store_load(&cfg) == ESP_OK) {
        cfg.mood_log_interval_minutes = minutes;
        (void)config_store_save(&cfg);
    }
    printf("Mood log interval set to %d minute(s)%s\n", minutes, minutes == 0 ? " (disabled)" : "");
    return 0;
}

static int cmd_emo_set(int argc, char **argv)
{
    if (!s_ctx) {
        printf("Emotion context not available\n");
        return 0;
    }
    if (argc < 3) {
        printf("Usage: emo_set <happy|satiety|energy|social|fear|eldritch|battery> <value>\n");
        printf("   or: emo_set all <happy> <satiety> <energy> <social> <fear> <eldritch> [battery]\n");
        return 0;
    }

    if (strcasecmp(argv[1], "all") == 0) {
        if (argc < 8) {
            printf("Usage: emo_set all <happy> <satiety> <energy> <social> <fear> <eldritch> [battery]\n");
            return 0;
        }
        s_ctx->happiness = (uint8_t)clamp_meter((int)strtol(argv[2], NULL, 10), 0, 100);
        s_ctx->hunger = (uint8_t)clamp_meter((int)strtol(argv[3], NULL, 10), 0, 130);
        s_ctx->energy = (uint8_t)clamp_meter((int)strtol(argv[4], NULL, 10), 0, 100);
        s_ctx->social = (uint8_t)clamp_meter((int)strtol(argv[5], NULL, 10), 0, 100);
        s_ctx->fear = (uint8_t)clamp_meter((int)strtol(argv[6], NULL, 10), 0, 100);
        s_ctx->eldritch_charge = (uint8_t)clamp_meter((int)strtol(argv[7], NULL, 10), 0, 100);
        if (argc >= 9) {
            emotion_set_battery_percent(s_ctx, (uint8_t)clamp_meter((int)strtol(argv[8], NULL, 10), 0, 100));
        }
    } else {
        int value = (int)strtol(argv[2], NULL, 10);
        if (strcasecmp(argv[1], "happy") == 0 || strcasecmp(argv[1], "happiness") == 0) {
            s_ctx->happiness = (uint8_t)clamp_meter(value, 0, 100);
        } else if (strcasecmp(argv[1], "sat") == 0 || strcasecmp(argv[1], "satiety") == 0 ||
                   strcasecmp(argv[1], "hunger") == 0) {
            s_ctx->hunger = (uint8_t)clamp_meter(value, 0, 130);
        } else if (strcasecmp(argv[1], "energy") == 0) {
            s_ctx->energy = (uint8_t)clamp_meter(value, 0, 100);
        } else if (strcasecmp(argv[1], "social") == 0) {
            s_ctx->social = (uint8_t)clamp_meter(value, 0, 100);
        } else if (strcasecmp(argv[1], "fear") == 0) {
            s_ctx->fear = (uint8_t)clamp_meter(value, 0, 100);
        } else if (strcasecmp(argv[1], "eld") == 0 || strcasecmp(argv[1], "eldritch") == 0) {
            s_ctx->eldritch_charge = (uint8_t)clamp_meter(value, 0, 100);
        } else if (strcasecmp(argv[1], "batt") == 0 || strcasecmp(argv[1], "battery") == 0) {
            emotion_set_battery_percent(s_ctx, (uint8_t)clamp_meter(value, 0, 100));
        } else {
            printf("Unknown meter '%s'\n", argv[1]);
            return 0;
        }
    }

    if (eldra_request_render_now) {
        eldra_request_render_now();
    }

    printf("Meters set: happy=%u satiety=%u energy=%u social=%u fear=%u eldritch=%u batt=%u%%\n",
           s_ctx->happiness, s_ctx->hunger, s_ctx->energy, s_ctx->social,
           s_ctx->fear, s_ctx->eldritch_charge, s_ctx->battery_percent);
    return 0;
}

static int cmd_eyes_sleep_force(int argc, char **argv)
{
    if (!eldra_set_sleep_eye_override) {
        printf("Sleep-eye override hook unavailable in this build\n");
        return 0;
    }

    uint8_t current = 0;
    if (eldra_get_sleep_eye_override) {
        current = eldra_get_sleep_eye_override();
    }

    if (argc < 2 || strcasecmp(argv[1], "status") == 0) {
        const char *name = (current == 0U) ? "off" : (current == 1U) ? "light" : "heavy";
        printf("Sleep-eye override: %s (%u)\n", name, (unsigned)current);
        printf("Usage: eyes_sleep_force <off|light|heavy|status>\n");
        return 0;
    }

    uint8_t mode = current;
    if (strcasecmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0) {
        mode = 0U;
    } else if (strcasecmp(argv[1], "light") == 0 || strcasecmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0) {
        mode = 1U;
    } else if (strcasecmp(argv[1], "heavy") == 0 || strcmp(argv[1], "2") == 0) {
        mode = 2U;
    } else {
        printf("Usage: eyes_sleep_force <off|light|heavy|status>\n");
        return 0;
    }

    eldra_set_sleep_eye_override(mode);
    const char *name = (mode == 0U) ? "off" : (mode == 1U) ? "light" : "heavy";
    printf("Sleep-eye override set: %s (%u)\n", name, (unsigned)mode);
    return 0;
}

static int cmd_emo_weight(int argc, char **argv)
{
    emotion_affect_weights_t w = {0};
    emotion_get_affect_weights(&w);

    if (argc < 2 || strcasecmp(argv[1], "status") == 0) {
        print_affect_weights();
        printf("Usage: emo_weight <happy|satiety|energy|social|fear> <pct 0..300>\n");
        printf("   or: emo_weight all <happy> <satiety> <energy> <social> <fear>\n");
        return 0;
    }

    if (strcasecmp(argv[1], "all") == 0) {
        if (argc < 7) {
            printf("Usage: emo_weight all <happy> <satiety> <energy> <social> <fear>\n");
            return 0;
        }
        w.happiness_pct = (uint16_t)clamp_weight_pct((int)strtol(argv[2], NULL, 10));
        w.satiety_pct = (uint16_t)clamp_weight_pct((int)strtol(argv[3], NULL, 10));
        w.energy_pct = (uint16_t)clamp_weight_pct((int)strtol(argv[4], NULL, 10));
        w.social_pct = (uint16_t)clamp_weight_pct((int)strtol(argv[5], NULL, 10));
        w.fear_pct = (uint16_t)clamp_weight_pct((int)strtol(argv[6], NULL, 10));
    } else {
        if (argc < 3) {
            printf("Usage: emo_weight <happy|satiety|energy|social|fear> <pct 0..300>\n");
            return 0;
        }
        uint16_t value = (uint16_t)clamp_weight_pct((int)strtol(argv[2], NULL, 10));
        if (strcasecmp(argv[1], "happy") == 0 || strcasecmp(argv[1], "happiness") == 0) {
            w.happiness_pct = value;
        } else if (strcasecmp(argv[1], "sat") == 0 || strcasecmp(argv[1], "satiety") == 0 ||
                   strcasecmp(argv[1], "hunger") == 0) {
            w.satiety_pct = value;
        } else if (strcasecmp(argv[1], "energy") == 0) {
            w.energy_pct = value;
        } else if (strcasecmp(argv[1], "social") == 0) {
            w.social_pct = value;
        } else if (strcasecmp(argv[1], "fear") == 0) {
            w.fear_pct = value;
        } else {
            printf("Unknown target '%s'\n", argv[1]);
            return 0;
        }
    }

    emotion_set_affect_weights(&w);
    config_store_t cfg = {0};
    if (config_store_load(&cfg) == ESP_OK) {
        cfg.affect_weight_happiness_pct = (int)w.happiness_pct;
        cfg.affect_weight_satiety_pct = (int)w.satiety_pct;
        cfg.affect_weight_energy_pct = (int)w.energy_pct;
        cfg.affect_weight_social_pct = (int)w.social_pct;
        cfg.affect_weight_fear_pct = (int)w.fear_pct;
        if (config_store_save(&cfg) != ESP_OK) {
            printf("Affect weights applied (persist failed)\n");
        }
    } else {
        printf("Affect weights applied (config load failed; not persisted)\n");
    }
    if (eldra_request_render_now) {
        eldra_request_render_now();
    }
    print_affect_weights();
    return 0;
}

static int cmd_eyes_offset(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: eyes_offset <x_px> <y_px> [persist|temp]\n");
        return 0;
    }
    int x = (int)strtol(argv[1], NULL, 10);
    int y = (int)strtol(argv[2], NULL, 10);
    bool persist = true;
    if (argc >= 4) {
        if (strcasecmp(argv[3], "temp") == 0 || strcasecmp(argv[3], "volatile") == 0) {
            persist = false;
        } else if (strcasecmp(argv[3], "persist") == 0) {
            persist = true;
        }
    }
    eldra_eyes_set_center_offset(x, y);
    int eff_x = x, eff_y = y;
    eldra_eyes_get_effective_center_offset(&eff_x, &eff_y);
    if (persist) {
        config_store_t cfg;
        if (config_store_load(&cfg) == ESP_OK) {
            cfg.eyes_center_x_offset = eff_x;
            cfg.eyes_center_y_offset = eff_y;
            if (config_store_save(&cfg) == ESP_OK) {
                printf("Eyes center offset persisted to config (requested x=%d y=%d, saved/effective x=%d y=%d)\n",
                       x, y, eff_x, eff_y);
            } else {
                printf("Persist failed (save error)\n");
            }
        } else {
            printf("Persist failed (config load error)\n");
        }
    } else {
        printf("Eyes center offset set (requested x=%d y=%d, effective x=%d y=%d; not persisted)\n",
               x, y, eff_x, eff_y);
    }
    return 0;
}

static int cmd_eyes_test(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: eyes_test on|off\n");
        return 0;
    }
    bool enable = false;
    if (strcasecmp(argv[1], "on") == 0 || strcasecmp(argv[1], "enable") == 0) {
        enable = true;
    } else if (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "disable") == 0) {
        enable = false;
    } else {
        printf("Usage: eyes_test on|off\n");
        return 0;
    }
    eldra_eyes_set_test_pattern(enable);
    printf("Eyes test pattern %s\n", enable ? "ENABLED" : "DISABLED");
    return 0;
}

static int cmd_eyes_lid(int argc, char **argv)
{
    uint8_t sleep_depth = 0;
    uint8_t angry_depth = 0;
    eldra_eyes_get_lid_depths(&sleep_depth, &angry_depth);

    if (argc == 1) {
        printf("Lid depths: sleep=%u angry=%u\n", (unsigned)sleep_depth, (unsigned)angry_depth);
        printf("Usage: eyes_lid <sleep_depth 0..6> [angry_depth 0..6] [persist|temp]\n");
        return 0;
    }

    char *end = NULL;
    int sleep_req = (int)strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0') {
        printf("Invalid sleep depth '%s'. Allowed range is 0..6\n", argv[1]);
        return 0;
    }

    int angry_req = (int)angry_depth;
    int mode_arg_index = -1;
    if (argc >= 3) {
        end = NULL;
        int parsed = (int)strtol(argv[2], &end, 10);
        if (end != argv[2] && *end == '\0') {
            angry_req = parsed;
            mode_arg_index = (argc >= 4) ? 3 : -1;
        } else {
            mode_arg_index = 2;
        }
    }

    bool persist = true;
    if (mode_arg_index > 0) {
        if (strcasecmp(argv[mode_arg_index], "temp") == 0 || strcasecmp(argv[mode_arg_index], "volatile") == 0) {
            persist = false;
        } else if (strcasecmp(argv[mode_arg_index], "persist") == 0) {
            persist = true;
        } else {
            printf("Invalid mode '%s'. Use persist|temp\n", argv[mode_arg_index]);
            return 0;
        }
    }

    if (sleep_req < 0 || sleep_req > 6 || angry_req < 0 || angry_req > 6) {
        printf("Invalid depth. Allowed range is 0..6\n");
        return 0;
    }

    eldra_eyes_set_lid_depths((uint8_t)sleep_req, (uint8_t)angry_req);
    eldra_eyes_get_lid_depths(&sleep_depth, &angry_depth);
    if (persist) {
        config_store_t cfg = {0};
        if (config_store_load(&cfg) == ESP_OK) {
            cfg.sleep_lid_depth = (int)sleep_depth;
            cfg.angry_lid_depth = (int)angry_depth;
            if (config_store_save(&cfg) == ESP_OK) {
                printf("Lid depths set and persisted: sleep=%u angry=%u\n", (unsigned)sleep_depth, (unsigned)angry_depth);
            } else {
                printf("Lid depths set (persist failed): sleep=%u angry=%u\n", (unsigned)sleep_depth, (unsigned)angry_depth);
            }
        } else {
            printf("Lid depths set (config load failed): sleep=%u angry=%u\n", (unsigned)sleep_depth, (unsigned)angry_depth);
        }
    } else {
        printf("Lid depths set (not persisted): sleep=%u angry=%u\n", (unsigned)sleep_depth, (unsigned)angry_depth);
    }
    return 0;
}

static int cmd_disp_center(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: disp_center <x_px> <y_px> [persist|temp]\n");
        return 0;
    }
    int x = (int)strtol(argv[1], NULL, 10);
    int y = (int)strtol(argv[2], NULL, 10);
    bool persist = true;
    if (argc >= 4) {
        if (strcasecmp(argv[3], "temp") == 0 || strcasecmp(argv[3], "volatile") == 0) {
            persist = false;
        } else if (strcasecmp(argv[3], "persist") == 0) {
            persist = true;
        }
    }
    eldra_eyes_set_display_center_offset(x, y);
    eldra_glyphs_set_display_center_offset(x, y);
    int raw_x = 0, raw_y = 0;
    int eff_x = 0, eff_y = 0;
    eldra_eyes_get_display_center_offset(&raw_x, &raw_y);
    eldra_eyes_get_effective_center_offset(&eff_x, &eff_y);
    if (persist) {
        config_store_t cfg;
        if (config_store_load(&cfg) == ESP_OK) {
            cfg.display_center_x_offset = x;
            cfg.display_center_y_offset = y;
            if (config_store_save(&cfg) == ESP_OK) {
                printf("Display center persisted (requested x=%d y=%d, applied x=%d y=%d, effective eyes x=%d y=%d)\n",
                       x, y, raw_x, raw_y, eff_x, eff_y);
            } else {
                printf("Persist failed (save error)\n");
            }
        } else {
            printf("Persist failed (config load error)\n");
        }
    } else {
        printf("Display center set (requested x=%d y=%d, applied x=%d y=%d, effective eyes x=%d y=%d; not persisted)\n",
               x, y, raw_x, raw_y, eff_x, eff_y);
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
        .help = "Pet Eldra. Usage: emo_pet [small|big]",
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

    const esp_console_cmd_t needs_cmd = {
        .command = "emo_needs",
        .help = "Show need bitmask (hungry/lonely/sleepy/etc.).",
        .hint = NULL,
        .func = &cmd_state_show,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&needs_cmd), TAG, "register emo_needs failed");

    const esp_console_cmd_t show_cmd = {
        .command = "emo_state_show",
        .help = "Show current emotion state/meters.",
        .hint = NULL,
        .func = &cmd_state_show,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&show_cmd), TAG, "register emo_state_show failed");

    const esp_console_cmd_t logint_cmd = {
        .command = "emo_log_interval",
        .help = "Set mood log interval in minutes (0 disables logging). Usage: emo_log_interval <minutes>",
        .hint = NULL,
        .func = &cmd_log_interval,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&logint_cmd), TAG, "register emo_log_interval failed");

    const esp_console_cmd_t meter_set_cmd = {
        .command = "emo_set",
        .help = "Set emotion meters for testing. Usage: emo_set <meter> <value> | emo_set all <h sat e s f eld> [batt]",
        .hint = NULL,
        .func = &cmd_emo_set,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&meter_set_cmd), TAG, "register emo_set failed");

    const esp_console_cmd_t weight_set_cmd = {
        .command = "emo_weight",
        .help = "Set affect weights (pct; 100=default). Usage: emo_weight <target> <pct> | emo_weight all <h sat e s f> | emo_weight status",
        .hint = NULL,
        .func = &cmd_emo_weight,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&weight_set_cmd), TAG, "register emo_weight failed");

    const esp_console_cmd_t offset_cmd = {
        .command = "eyes_offset",
        .help = "Set eye center offset in pixels (persists by default). Usage: eyes_offset <x> <y> [persist|temp] (positive=right/down)",
        .hint = NULL,
        .func = &cmd_eyes_offset,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&offset_cmd), TAG, "register eyes_offset failed");

    const esp_console_cmd_t test_cmd = {
        .command = "eyes_test",
        .help = "Toggle eyes test pattern (crosshair/border) for centering. Usage: eyes_test on|off",
        .hint = NULL,
        .func = &cmd_eyes_test,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&test_cmd), TAG, "register eyes_test failed");

    const esp_console_cmd_t lid_cmd = {
        .command = "eyes_lid",
        .help = "Set/persist lid depth. Usage: eyes_lid <sleep_depth 0..6> [angry_depth 0..6] [persist|temp]",
        .hint = NULL,
        .func = &cmd_eyes_lid,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&lid_cmd), TAG, "register eyes_lid failed");

    const esp_console_cmd_t sleep_force_cmd = {
        .command = "eyes_sleep_force",
        .help = "Force sleepy-eye overlay without entering sleep. Usage: eyes_sleep_force <off|light|heavy|status>",
        .hint = NULL,
        .func = &cmd_eyes_sleep_force,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&sleep_force_cmd), TAG, "register eyes_sleep_force failed");

    const esp_console_cmd_t disp_center_cmd = {
        .command = "disp_center",
        .help = "Set display center offset (persists by default). Usage: disp_center <x> <y> [persist|temp]",
        .hint = NULL,
        .func = &cmd_disp_center,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&disp_center_cmd), TAG, "register disp_center failed");

    return ESP_OK;
}
