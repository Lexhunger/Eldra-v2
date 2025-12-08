#include "eldra_emotion.h"

#include <stddef.h>
#include <string.h>

#include "eldra_logging.h"

/**
 * @brief Meter limits for all tracked needs.
 */
#define EMOTION_METER_MIN 0
#define EMOTION_METER_MAX 100

/**
 * @brief Baseline meter defaults applied at init.
 */
#define BASELINE_HAPPINESS 70
#define BASELINE_HUNGER 20
#define BASELINE_ENERGY 80
#define BASELINE_SOCIAL 60
#define BASELINE_FEAR 10
#define BASELINE_ELDRITCH 10

/**
 * @brief Passive rate constants per second.
 * Keep rates small so minutes, not milliseconds, drive the bigger mood swings.
 */
#define HUNGER_RISE_PER_SEC 1U
#define SOCIAL_DECAY_PER_SEC 1U
#define ENERGY_RECOVER_PER_SEC 1U
#define FEAR_DECAY_PER_SEC 1U
#define ELDRITCH_CHARGE_RISE_MS 5000U /* +1 charge per 5 seconds */

/**
 * @brief Event-driven meter deltas.
 */
#define FEED_HUNGER_DROP 30
#define FEED_HAPPINESS_BOOST 8
#define FEED_ENERGY_BOOST 5

#define PET_HAPPINESS_BOOST 6
#define PET_SOCIAL_BOOST 10
#define PET_FEAR_DROP 8

#define PLAY_HAPPINESS_BOOST 10
#define PLAY_SOCIAL_BOOST 10
#define PLAY_ENERGY_COST 12
#define PLAY_HUNGER_RISE 8

#define SHAKE_FEAR_BOOST 12
#define SHAKE_ELDRITCH_BOOST 3

#define EDGE_FEAR_SPIKE 25
#define EDGE_HAPPINESS_DROP 6

#define HOOD_CLOSED_ENERGY_RECOVERY 4
#define HOOD_CLOSED_SOCIAL_DROP 4
#define HOOD_OPEN_SOCIAL_BOOST 4

#define VOICE_HAPPINESS_BOOST 4
#define VOICE_SOCIAL_BOOST 4

/**
 * @brief Timer durations.
 */
#define DIZZY_BASE_DURATION_MS 5000U
#define DIZZY_INTENSITY_SCALE_MS 200U
#define SCARED_DURATION_MS 4000U

/**
 * @brief State thresholds.
 * Tuned to keep "needs" states (hungry, sleepy, lonely) above feel-good states.
 */
#define THRESHOLD_FEAR_SCARED 75
#define THRESHOLD_ELDRITCH 80
#define THRESHOLD_HUNGER 70
#define THRESHOLD_ENERGY_SLEEPY 25
#define THRESHOLD_SOCIAL_LONELY 25
#define THRESHOLD_HAPPINESS_SAD 30
#define THRESHOLD_HAPPINESS_HAPPY 70
#define THRESHOLD_SAD_SUPPORT_HUNGER 60
#define THRESHOLD_SAD_SUPPORT_SOCIAL 40
#define THRESHOLD_SAD_SUPPORT_ENERGY 30
#define THRESHOLD_PLAYFUL_RECENT_MS 30000U
#define THRESHOLD_PLAYFUL_HAPPINESS 70
#define THRESHOLD_PLAYFUL_SOCIAL 60

static const char *TAG = "emotion";

static uint8_t clamp_meter_int(int32_t value) {
    if (value < EMOTION_METER_MIN) {
        return (uint8_t)EMOTION_METER_MIN;
    }
    if (value > EMOTION_METER_MAX) {
        return (uint8_t)EMOTION_METER_MAX;
    }
    return (uint8_t)value;
}

static void apply_delta(uint8_t *meter, int32_t delta) {
    if (!meter) {
        return;
    }
    int32_t updated = (int32_t)(*meter) + delta;
    *meter = clamp_meter_int(updated);
}

static uint32_t elapsed_ms(uint32_t start_ms, uint32_t now_ms) {
    return now_ms - start_ms; // unsigned wrap-friendly
}

static bool timer_active(uint32_t now_ms, uint32_t until_ms) {
    if (until_ms == 0U) {
        return false;
    }
    return ((int32_t)(until_ms - now_ms)) > 0;
}

static const char *state_name(emotion_state_t state) {
    switch (state) {
        case EMOTION_STATE_NEUTRAL:
            return "NEUTRAL";
        case EMOTION_STATE_HAPPY:
            return "HAPPY";
        case EMOTION_STATE_SAD:
            return "SAD";
        case EMOTION_STATE_LONELY:
            return "LONELY";
        case EMOTION_STATE_SLEEPY:
            return "SLEEPY";
        case EMOTION_STATE_HUNGRY:
            return "HUNGRY";
        case EMOTION_STATE_PLAYFUL:
            return "PLAYFUL";
        case EMOTION_STATE_ELDRITCH:
            return "ELDRITCH";
        case EMOTION_STATE_SCARED:
            return "SCARED";
        case EMOTION_STATE_DIZZY:
            return "DIZZY";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief Evaluate the current meters/timers to select a named state. Priority runs
 *        from high-urgency (dizzy/scared) down through needs (hungry/sleepy/lonely)
 *        and finally into positive states (playful/happy) before the neutral fallback.
 */
static emotion_state_t emotion_select_state(const emotion_context_t *ctx, uint32_t now_ms) {
    bool dizzy = ctx->dizzy_active && timer_active(now_ms, ctx->dizzy_until_ms);
    bool scared = ctx->scared_active && timer_active(now_ms, ctx->scared_until_ms);
    bool recent_play = elapsed_ms(ctx->last_play_ms, now_ms) <= THRESHOLD_PLAYFUL_RECENT_MS;

    if (dizzy) {
        return EMOTION_STATE_DIZZY;
    }
    if (scared || ctx->fear >= THRESHOLD_FEAR_SCARED) {
        return EMOTION_STATE_SCARED;
    }
    if (ctx->eldritch_charge >= THRESHOLD_ELDRITCH) {
        return EMOTION_STATE_ELDRITCH;
    }
    if (ctx->hunger >= THRESHOLD_HUNGER) {
        return EMOTION_STATE_HUNGRY;
    }
    if (ctx->energy <= THRESHOLD_ENERGY_SLEEPY) {
        return EMOTION_STATE_SLEEPY;
    }
    if (ctx->social <= THRESHOLD_SOCIAL_LONELY) {
        return EMOTION_STATE_LONELY;
    }
    if (ctx->happiness <= THRESHOLD_HAPPINESS_SAD &&
        (ctx->hunger >= THRESHOLD_SAD_SUPPORT_HUNGER ||
         ctx->social <= THRESHOLD_SAD_SUPPORT_SOCIAL ||
         ctx->energy <= THRESHOLD_SAD_SUPPORT_ENERGY)) {
        return EMOTION_STATE_SAD;
    }
    if (ctx->happiness >= THRESHOLD_PLAYFUL_HAPPINESS && ctx->social >= THRESHOLD_PLAYFUL_SOCIAL &&
        recent_play) {
        return EMOTION_STATE_PLAYFUL;
    }
    if (ctx->happiness >= THRESHOLD_HAPPINESS_HAPPY) {
        return EMOTION_STATE_HAPPY;
    }
    return EMOTION_STATE_NEUTRAL;
}

static void handle_state_transition(emotion_context_t *ctx, emotion_state_t next_state, uint32_t now_ms) {
    if (!ctx) {
        return;
    }
    if (next_state == ctx->current_state) {
        return;
    }
    emotion_state_t previous = ctx->current_state;
    ctx->previous_state = previous;
    ctx->current_state = next_state;
    ctx->last_state_change_ms = now_ms;
    emotion_notify_state_change(previous, next_state);
}

void emotion_init(emotion_context_t *ctx, uint32_t now_ms) {
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->happiness = BASELINE_HAPPINESS;
    ctx->hunger = BASELINE_HUNGER;
    ctx->energy = BASELINE_ENERGY;
    ctx->social = BASELINE_SOCIAL;
    ctx->fear = BASELINE_FEAR;
    ctx->eldritch_charge = BASELINE_ELDRITCH;

    ctx->current_state = EMOTION_STATE_NEUTRAL;
    ctx->previous_state = EMOTION_STATE_NEUTRAL;
    ctx->last_state_change_ms = now_ms;
    ctx->last_tick_ms = now_ms;
    ctx->tick_accum_ms = 0U;
    ctx->eldritch_accum_ms = 0U;
}

void emotion_on_tick(emotion_context_t *ctx, uint32_t now_ms) {
    if (!ctx) {
        return;
    }
    uint32_t dt_ms = elapsed_ms(ctx->last_tick_ms, now_ms);
    ctx->last_tick_ms = now_ms;

    uint32_t total_ms = dt_ms + ctx->tick_accum_ms;
    uint32_t seconds = total_ms / 1000U;
    ctx->tick_accum_ms = total_ms % 1000U;

    if (seconds > 0U) {
        // Passive drift: needs creep toward hungry/lonely unless refreshed by events.
        apply_delta(&ctx->hunger, (int32_t)(HUNGER_RISE_PER_SEC * seconds));
        apply_delta(&ctx->social, -((int32_t)SOCIAL_DECAY_PER_SEC * (int32_t)seconds));
        apply_delta(&ctx->energy, ((int32_t)ENERGY_RECOVER_PER_SEC * (int32_t)seconds));
        apply_delta(&ctx->fear, -((int32_t)FEAR_DECAY_PER_SEC * (int32_t)seconds));
    }

    uint32_t eldritch_total = dt_ms + ctx->eldritch_accum_ms;
    uint32_t eldritch_steps = eldritch_total / ELDRITCH_CHARGE_RISE_MS;
    ctx->eldritch_accum_ms = eldritch_total % ELDRITCH_CHARGE_RISE_MS;
    if (eldritch_steps > 0U) {
        apply_delta(&ctx->eldritch_charge, (int32_t)eldritch_steps);
    }

    ctx->dizzy_active = timer_active(now_ms, ctx->dizzy_until_ms);
    ctx->scared_active = timer_active(now_ms, ctx->scared_until_ms);

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

void emotion_on_feed(emotion_context_t *ctx, uint32_t food_type, uint32_t now_ms) {
    (void)food_type;
    if (!ctx) {
        return;
    }
    apply_delta(&ctx->hunger, -FEED_HUNGER_DROP);
    apply_delta(&ctx->happiness, FEED_HAPPINESS_BOOST);
    apply_delta(&ctx->energy, FEED_ENERGY_BOOST);
    ctx->last_feed_ms = now_ms;

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

void emotion_on_pet(emotion_context_t *ctx, uint32_t now_ms) {
    if (!ctx) {
        return;
    }
    apply_delta(&ctx->happiness, PET_HAPPINESS_BOOST);
    apply_delta(&ctx->social, PET_SOCIAL_BOOST);
    apply_delta(&ctx->fear, -PET_FEAR_DROP);
    ctx->last_pet_ms = now_ms;

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

void emotion_on_play(emotion_context_t *ctx, uint32_t now_ms) {
    if (!ctx) {
        return;
    }
    apply_delta(&ctx->happiness, PLAY_HAPPINESS_BOOST);
    apply_delta(&ctx->social, PLAY_SOCIAL_BOOST);
    apply_delta(&ctx->energy, -PLAY_ENERGY_COST);
    apply_delta(&ctx->hunger, PLAY_HUNGER_RISE);
    ctx->last_play_ms = now_ms;

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

void emotion_on_shake(emotion_context_t *ctx, int intensity, uint32_t now_ms) {
    if (!ctx) {
        return;
    }
    int32_t bounded_intensity = intensity;
    if (bounded_intensity < 0) {
        bounded_intensity = 0;
    }
    apply_delta(&ctx->fear, SHAKE_FEAR_BOOST + bounded_intensity);
    apply_delta(&ctx->eldritch_charge, SHAKE_ELDRITCH_BOOST);
    uint32_t extra = (uint32_t)bounded_intensity * DIZZY_INTENSITY_SCALE_MS;
    ctx->dizzy_until_ms = now_ms + DIZZY_BASE_DURATION_MS + extra;
    ctx->dizzy_active = true;
    ctx->last_shake_ms = now_ms;

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

void emotion_on_edge_detected(emotion_context_t *ctx, uint32_t now_ms) {
    if (!ctx) {
        return;
    }
    apply_delta(&ctx->fear, EDGE_FEAR_SPIKE);
    apply_delta(&ctx->happiness, -EDGE_HAPPINESS_DROP);
    ctx->scared_until_ms = now_ms + SCARED_DURATION_MS;
    ctx->scared_active = true;
    ctx->last_edge_ms = now_ms;

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

void emotion_on_hood_state_changed(emotion_context_t *ctx, bool hood_closed, uint32_t now_ms) {
    if (!ctx) {
        return;
    }
    ctx->hood_closed = hood_closed;
    if (hood_closed) {
        apply_delta(&ctx->energy, HOOD_CLOSED_ENERGY_RECOVERY);
        apply_delta(&ctx->social, -HOOD_CLOSED_SOCIAL_DROP);
    } else {
        apply_delta(&ctx->social, HOOD_OPEN_SOCIAL_BOOST);
    }

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

void emotion_on_voice_command(emotion_context_t *ctx, uint32_t phrase_id, uint32_t now_ms) {
    (void)phrase_id;
    if (!ctx) {
        return;
    }
    apply_delta(&ctx->happiness, VOICE_HAPPINESS_BOOST);
    apply_delta(&ctx->social, VOICE_SOCIAL_BOOST);
    ctx->last_voice_ms = now_ms;

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

emotion_state_t emotion_get_state(const emotion_context_t *ctx) {
    if (!ctx) {
        return EMOTION_STATE_NEUTRAL;
    }
    return ctx->current_state;
}

__attribute__((weak)) void emotion_notify_state_change(emotion_state_t old_state,
                                                       emotion_state_t new_state) {
    log_event(LOG_LEVEL_INFO, TAG, "State changed %s -> %s", state_name(old_state), state_name(new_state));
}
