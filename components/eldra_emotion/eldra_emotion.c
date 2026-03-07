#include "eldra_emotion.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

#include "eldra_logging.h"
#include "esp_random.h"

/**
 * @brief Meter limits for all tracked needs.
 */
#define EMOTION_METER_MIN 0
#define EMOTION_METER_MAX 100
// Hunger (satiety) can exceed 100 briefly for "stuffed/food coma" handling.
#define EMOTION_HUNGER_MAX 130

/**
 * @brief Baseline meter defaults applied at init.
 */
#define BASELINE_HAPPINESS 70
#define BASELINE_HUNGER 70  // satiety: higher = fuller; 70 sits mid "satisfied"
#define BASELINE_ENERGY 80
#define BASELINE_SOCIAL 70
#define BASELINE_FEAR 10
#define BASELINE_ELDRITCH 10

/**
 * @brief Passive rate constants per second.
 * Keep rates small so minutes, not milliseconds, drive the bigger mood swings.
 */
#define HUNGER_DECAY_SEC_PER_POINT 1080U // ~18 min per satiety point -> ~24h to drop ~80 points
#define SOCIAL_DECAY_SEC_PER_POINT 900U  // ~15 min per social point
#define ENERGY_DECAY_SEC_PER_POINT 900U  // ~15 min per energy point (drops ~60 in 15h)
#define ENERGY_RECOVER_SLEEP_SEC_PER_POINT 300U // ~5 min per energy point while sleeping/resting
#define FEAR_DECAY_PER_SEC 1U
#define INACTIVITY_ELDRITCH_START_MS 300000U /* 5 minutes idle before decay */
#define INACTIVITY_ELDRITCH_DECAY_SEC 180U   /* then -1 every 3 minutes */

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
#define SHAKE_ELDRITCH_BOOST 0
#define PLAY_ELDRITCH_BOOST 6
#define PET_ELDRITCH_BIG_BOOST 2

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
#define THRESHOLD_ELDRITCH 96
#define THRESHOLD_HUNGER 30   // <= this is hungry
#define THRESHOLD_HUNGER_HANGRY 15
#define THRESHOLD_ENERGY_SLEEPY 30
#define THRESHOLD_ENERGY_EXHAUSTED 10
#define THRESHOLD_BATTERY_SLEEPY 20
#define THRESHOLD_SOCIAL_LONELY 25
#define THRESHOLD_SOCIAL_ISOLATED 10
#define THRESHOLD_HAPPINESS_SAD 30
#define THRESHOLD_HAPPINESS_HAPPY 70
#define THRESHOLD_SAD_SUPPORT_HUNGER 60
#define THRESHOLD_SAD_SUPPORT_SOCIAL 40
#define THRESHOLD_SAD_SUPPORT_ENERGY 30
#define THRESHOLD_ELDRITCH_GATED_HUNGER 55
#define THRESHOLD_ELDRITCH_GATED_SLEEPY 35
#define THRESHOLD_ELDRITCH_SUPER_HAPPY 88
#define THRESHOLD_ELDRITCH_SUPER_SOCIAL 85
#define THRESHOLD_ELDRITCH_SUPER_ENERGY 70
#define THRESHOLD_ELDRITCH_SUPER_BATTERY 40
#define THRESHOLD_ELDRITCH_SUPER_FEAR_MAX 20
#define THRESHOLD_ELDRITCH_RECENT_PLAY_MS 12000U
#define THRESHOLD_ELDRITCH_RECENT_PET_MS 12000U
#define THRESHOLD_PLAYFUL_RECENT_MS 30000U
#define THRESHOLD_PLAYFUL_HAPPINESS 70
#define THRESHOLD_PLAYFUL_SOCIAL 60
#define THRESHOLD_ANGRY_HUNGER THRESHOLD_HUNGER
#define THRESHOLD_ANGRY_TIRED_ENERGY THRESHOLD_ENERGY_SLEEPY
#define THRESHOLD_ANGRY_BORED_SOCIAL THRESHOLD_SOCIAL_LONELY
#define THRESHOLD_ANGRY_REALLY_BORED_SOCIAL THRESHOLD_SOCIAL_ISOLATED
#define SLEEP_IDLE_MS 300000U // 5 minutes of no interaction triggers sleep during quiet hours
// Sleep window and mood log interval are configurable via setters.
static uint8_t s_sleep_start_hour = 22; // 10 PM
static uint8_t s_sleep_end_hour = 8;    // 8 AM
static uint32_t s_mood_log_interval_ms = 5 * 60 * 1000U; // default 5 minutes
static uint8_t s_angry_dizzy_count_threshold = 3;         // trigger on >3 dizzy events
static uint32_t s_angry_dizzy_window_ms = 5 * 60 * 1000U; // 5-minute rolling window
static uint32_t s_angry_override_min_ms = 2 * 60 * 1000U; // 2 minutes
static uint32_t s_angry_override_max_ms = 5 * 60 * 1000U; // 5 minutes

static const char *TAG = "emotion";

static emotion_affect_weights_t s_affect_weights = {
    .happiness_pct = 100,
    .satiety_pct = 100,
    .energy_pct = 100,
    .social_pct = 100,
    .fear_pct = 100,
};

static uint16_t clamp_weight_pct(uint16_t pct)
{
    if (pct > 300U) {
        return 300U;
    }
    return pct;
}

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

static uint8_t clamp_hunger(int32_t value) {
    if (value < EMOTION_METER_MIN) return (uint8_t)EMOTION_METER_MIN;
    if (value > EMOTION_HUNGER_MAX) return (uint8_t)EMOTION_HUNGER_MAX;
    return (uint8_t)value;
}

static void apply_delta_hunger(uint8_t *meter, int32_t delta) {
    if (!meter) return;
    int32_t updated = (int32_t)(*meter) + delta;
    *meter = clamp_hunger(updated);
}

static bool is_sleep_window(void) {
    // Quiet hours are local-time based; RTC must be set elsewhere.
    time_t t = time(NULL);
    struct tm tm_now;
    localtime_r(&t, &tm_now);
    int h = tm_now.tm_hour;
    if (h >= s_sleep_start_hour || h < s_sleep_end_hour) {
        return true;
    }
    return false;
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
        case EMOTION_STATE_ANGRY:
            return "ANGRY";
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

static uint32_t random_between_u32(uint32_t min_v, uint32_t max_v)
{
    if (max_v <= min_v) {
        return min_v;
    }
    uint32_t range = max_v - min_v;
    return min_v + (esp_random() % (range + 1U));
}

static bool is_eldritch_supercharged(const emotion_context_t *ctx, uint32_t now_ms)
{
    bool recent_play = elapsed_ms(ctx->last_play_ms, now_ms) <= THRESHOLD_ELDRITCH_RECENT_PLAY_MS;
    bool recent_pet = elapsed_ms(ctx->last_pet_ms, now_ms) <= THRESHOLD_ELDRITCH_RECENT_PET_MS;

    return (ctx->eldritch_charge >= THRESHOLD_ELDRITCH) &&
           recent_play &&
           recent_pet &&
           (ctx->happiness >= THRESHOLD_ELDRITCH_SUPER_HAPPY) &&
           (ctx->social >= THRESHOLD_ELDRITCH_SUPER_SOCIAL) &&
           (ctx->energy >= THRESHOLD_ELDRITCH_SUPER_ENERGY) &&
           (ctx->hunger >= THRESHOLD_ELDRITCH_GATED_HUNGER) &&
           (ctx->battery_percent >= THRESHOLD_ELDRITCH_SUPER_BATTERY) &&
           (ctx->fear <= THRESHOLD_ELDRITCH_SUPER_FEAR_MAX);
}

static emotion_need_mask_t compute_need_mask(const emotion_context_t *ctx, uint32_t now_ms) {
    emotion_need_mask_t mask = EMO_NEED_NONE;
    if (ctx->hunger <= THRESHOLD_HUNGER) mask |= EMO_NEED_HUNGRY;
    if (ctx->social <= THRESHOLD_SOCIAL_LONELY) mask |= EMO_NEED_LONELY;
    if (ctx->energy <= THRESHOLD_ENERGY_SLEEPY || ctx->battery_percent <= THRESHOLD_BATTERY_SLEEPY) mask |= EMO_NEED_SLEEPY;
    if (ctx->fear >= THRESHOLD_FEAR_SCARED || ctx->scared_active) mask |= EMO_NEED_SCARED;
    if (ctx->happiness >= THRESHOLD_PLAYFUL_HAPPINESS && ctx->social >= THRESHOLD_PLAYFUL_SOCIAL) mask |= EMO_NEED_PLAYFUL;
    if (is_eldritch_supercharged(ctx, now_ms)) mask |= EMO_NEED_ELDRITCH_READY;
    return mask;
}

static emotion_affect_t compute_affect(const emotion_context_t *ctx, int8_t *valence_out, int8_t *arousal_out) {
    // Simple heuristic: valence driven by happiness/social/satiety vs hunger/fear; arousal by energy/fear.
    int val = 0;
    int c_happy = ((int)ctx->happiness - 50);      // happiness strong contributor
    int c_social = ((int)ctx->social - 50) / 2;    // social moderate
    int c_sat = ((int)ctx->hunger - 50) / 2;       // satiety moderate
    int c_fear_val = ((int)ctx->fear - 20);        // fear negative
    val += (c_happy * (int)s_affect_weights.happiness_pct) / 100;
    val += (c_social * (int)s_affect_weights.social_pct) / 100;
    val += (c_sat * (int)s_affect_weights.satiety_pct) / 100;
    val -= (c_fear_val * (int)s_affect_weights.fear_pct) / 100;
    val = (val < -100) ? -100 : (val > 100 ? 100 : val);

    int aro = 0;
    int c_energy = ((int)ctx->energy - 50);        // energy strong
    int c_fear_aro = ((int)ctx->fear - 20) / 2;    // fear raises arousal
    int c_sat_aro = ((int)(50 - ctx->hunger)) / 3; // hunger (low satiety) saps arousal
    aro += (c_energy * (int)s_affect_weights.energy_pct) / 100;
    aro += (c_fear_aro * (int)s_affect_weights.fear_pct) / 100;
    aro -= (c_sat_aro * (int)s_affect_weights.satiety_pct) / 100;
    aro = (aro < -100) ? -100 : (aro > 100 ? 100 : aro);

    if (valence_out) *valence_out = (int8_t)val;
    if (arousal_out) *arousal_out = (int8_t)aro;

    if (val > 20 && aro > 20) return EMO_AFFECT_EXCITED;
    if (val > 10) return EMO_AFFECT_HAPPY;
    if (val < -30 && aro > 10) return EMO_AFFECT_ANGRY;
    if (val < -10) return EMO_AFFECT_SAD;
    return EMO_AFFECT_NEUTRAL;
}

/**
 * @brief Evaluate the current meters/timers to select a named state. Priority runs
 *        from high-urgency (dizzy/scared) down through needs (hungry/sleepy/lonely)
 *        and finally into positive states (playful/happy) before the neutral fallback.
 */
static emotion_state_t emotion_select_state(const emotion_context_t *ctx, uint32_t now_ms) {
    // Priority ladder: timers (dizzy/scared) -> forced sleep -> health gates -> needs -> positives.
    bool dizzy = ctx->dizzy_active && timer_active(now_ms, ctx->dizzy_until_ms);
    bool scared = ctx->scared_active && timer_active(now_ms, ctx->scared_until_ms);
    bool recent_play = elapsed_ms(ctx->last_play_ms, now_ms) <= THRESHOLD_PLAYFUL_RECENT_MS;

    // Cache composite needs for blending.
    ((emotion_context_t *)ctx)->active_needs = compute_need_mask(ctx, now_ms);

    if (dizzy) {
        return EMOTION_STATE_DIZZY;
    }
    if (scared || ctx->fear >= THRESHOLD_FEAR_SCARED) {
        return EMOTION_STATE_SCARED;
    }
    if (ctx->sleep_forced) {
        return EMOTION_STATE_SLEEPY;
    }
    if (timer_active(now_ms, ctx->angry_until_ms)) {
        return EMOTION_STATE_ANGRY;
    }
    bool food_coma = ctx->hunger > 100;
    if (food_coma) {
        return EMOTION_STATE_SLEEPY; // food coma overrides
    }
    if (ctx->hunger == 0) {
        return EMOTION_STATE_SLEEPY; // stasis until fed
    }
    if (ctx->hunger <= THRESHOLD_ANGRY_HUNGER) {
        // Requested angry triggers:
        // 1) hungry + tired + bored
        // 2) really bored + hungry
        bool hungry_tired_bored =
            (ctx->energy <= THRESHOLD_ANGRY_TIRED_ENERGY) &&
            (ctx->social <= THRESHOLD_ANGRY_BORED_SOCIAL);
        bool really_bored_hungry =
            (ctx->social <= THRESHOLD_ANGRY_REALLY_BORED_SOCIAL);
        if (hungry_tired_bored || really_bored_hungry || ctx->hunger <= THRESHOLD_HUNGER_HANGRY) {
            return EMOTION_STATE_ANGRY;
        }
        return EMOTION_STATE_HUNGRY;
    }
    if (ctx->energy <= THRESHOLD_ENERGY_EXHAUSTED || ctx->battery_percent <= THRESHOLD_BATTERY_SLEEPY) {
        return EMOTION_STATE_SLEEPY;
    }
    if (ctx->energy <= THRESHOLD_ENERGY_SLEEPY) {
        return EMOTION_STATE_SLEEPY;
    }
    if (ctx->social <= THRESHOLD_SOCIAL_ISOLATED) {
        return EMOTION_STATE_SAD;
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
    // Eldritch is intentionally ultra-rare and should only appear during
    // short, super-charged excitement windows.
    if (is_eldritch_supercharged(ctx, now_ms)) {
        return EMOTION_STATE_ELDRITCH;
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
    ctx->battery_percent = EMOTION_METER_MAX;

    ctx->current_state = EMOTION_STATE_NEUTRAL;
    ctx->previous_state = EMOTION_STATE_NEUTRAL;
    ctx->last_state_change_ms = now_ms;
    ctx->last_tick_ms = now_ms;
    ctx->tick_accum_ms = 0U;
    ctx->eldritch_accum_ms = 0U;
    ctx->last_interaction_ms = now_ms;
    ctx->last_mood_log_ms = now_ms;
    ctx->active_needs = EMO_NEED_NONE;
    ctx->affect = EMO_AFFECT_NEUTRAL;
    ctx->sleep_forced = false;
    ctx->angry_until_ms = 0;
    ctx->dizzy_burst_window_start_ms = 0;
    ctx->dizzy_burst_count = 0;
}

void emotion_set_battery_percent(emotion_context_t *ctx, uint8_t percent) {
    if (!ctx) {
        return;
    }
    ctx->battery_percent = clamp_meter_int(percent);
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
    uint32_t idle_ms = elapsed_ms(ctx->last_interaction_ms, now_ms);

    if (seconds > 0U) {
        // Passive drift: needs creep unless refreshed by events.
        // Satiety decays slowly toward hunger: drop 1 point every ~18 minutes.
        static uint32_t hunger_accum_sec = 0;
        static uint32_t social_accum_sec = 0;
        static uint32_t energy_accum_sec = 0;
        static uint32_t sleep_recover_sec = 0;
        static uint32_t eldritch_idle_accum_sec = 0;
        hunger_accum_sec += seconds;
        social_accum_sec += seconds;
        energy_accum_sec += seconds;
        sleep_recover_sec += seconds;
        if (idle_ms >= INACTIVITY_ELDRITCH_START_MS) {
            eldritch_idle_accum_sec += seconds;
        } else {
            eldritch_idle_accum_sec = 0;
        }
        while (hunger_accum_sec >= HUNGER_DECAY_SEC_PER_POINT) {
            apply_delta_hunger(&ctx->hunger, -1);
            hunger_accum_sec -= HUNGER_DECAY_SEC_PER_POINT;
        }
        while (social_accum_sec >= SOCIAL_DECAY_SEC_PER_POINT) {
            apply_delta(&ctx->social, -1);
            social_accum_sec -= SOCIAL_DECAY_SEC_PER_POINT;
        }
        while (energy_accum_sec >= ENERGY_DECAY_SEC_PER_POINT) {
            apply_delta(&ctx->energy, -1);
            energy_accum_sec -= ENERGY_DECAY_SEC_PER_POINT;
        }
        while (eldritch_idle_accum_sec >= INACTIVITY_ELDRITCH_DECAY_SEC) {
            apply_delta(&ctx->eldritch_charge, -1);
            apply_delta(&ctx->happiness, -1); // boredom creeps in
            eldritch_idle_accum_sec -= INACTIVITY_ELDRITCH_DECAY_SEC;
        }
        // Sleep recovery: if forced asleep or already sleepy, regain energy faster.
        if (ctx->sleep_forced || ctx->current_state == EMOTION_STATE_SLEEPY) {
            while (sleep_recover_sec >= ENERGY_RECOVER_SLEEP_SEC_PER_POINT && ctx->energy < 90) {
                apply_delta(&ctx->energy, +1);
                sleep_recover_sec -= ENERGY_RECOVER_SLEEP_SEC_PER_POINT;
            }
        } else {
            sleep_recover_sec = 0; // reset when awake/active
        }
        apply_delta(&ctx->fear, -((int32_t)FEAR_DECAY_PER_SEC * (int32_t)seconds));
    }

    // No passive eldritch growth: this meter is driven by explicit
    // interaction events only (play/pet/shake). Keep accumulator cleared.
    ctx->eldritch_accum_ms = 0U;

    // Fullness penalty: if stuffed (>90 satiety) drain energy slowly; >100 triggers coma via state above.
    if (ctx->hunger > 90) {
        int over = (int)ctx->hunger - 90;
        int32_t energy_penalty = (int32_t)((over * 2U * seconds) / 10U); // ~0.2 per second per point over 90
        if (energy_penalty > 0) {
            apply_delta(&ctx->energy, -energy_penalty);
        }
    }

    ctx->dizzy_active = timer_active(now_ms, ctx->dizzy_until_ms);
    ctx->scared_active = timer_active(now_ms, ctx->scared_until_ms);

    // Quiet-hour auto-sleep if idle.
    if (is_sleep_window()) {
        uint32_t idle_ms = elapsed_ms(ctx->last_interaction_ms, now_ms);
        if (idle_ms >= SLEEP_IDLE_MS) {
            if (!ctx->sleep_forced) {
                ctx->sleep_forced = true;
                log_event(LOG_LEVEL_INFO, TAG, "Auto-sleep: idle=%ums within window (%u-%u)",
                          (unsigned)idle_ms, (unsigned)s_sleep_start_hour, (unsigned)s_sleep_end_hour);
            }
        }
    }

    // Update affect snapshot for renderers.
    ctx->affect = compute_affect(ctx, NULL, NULL);

    // Periodic mood log to SD/console (configurable; 0 disables).
    if (s_mood_log_interval_ms > 0) {
        uint32_t delta = elapsed_ms(ctx->last_mood_log_ms, now_ms);
        // Guarantee a first log soon after boot even if interval is large.
        bool due_initial = (ctx->last_mood_log_ms == 0 && delta >= 5000U);
        if (delta >= s_mood_log_interval_ms || due_initial) {
            int8_t val = 0, aro = 0;
            emotion_affect_t aff = compute_affect(ctx, &val, &aro);
            emotion_need_mask_t needs = ctx->active_needs;
            log_event(LOG_LEVEL_INFO, TAG,
                      "mood meters happy=%u sat=%u energy=%u social=%u fear=%u eldritch=%u batt=%u%% affect=%d val=%d aro=%d needs=0x%02X",
                      ctx->happiness, ctx->hunger, ctx->energy, ctx->social, ctx->fear,
                      ctx->eldritch_charge, ctx->battery_percent, (int)aff, (int)val, (int)aro, (unsigned)needs);
            ctx->last_mood_log_ms = now_ms;
        }
    }

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

void emotion_on_feed(emotion_context_t *ctx, uint32_t food_type, uint32_t now_ms) {
    (void)food_type;
    if (!ctx) {
        return;
    }
    int32_t meal = FEED_HUNGER_DROP; // default big meal +60 satiety
    if (food_type == 1) {
        meal = 30; // small meal
    } else if (food_type >= 2) {
        meal = 5; // snack
    }
    apply_delta_hunger(&ctx->hunger, meal);
    apply_delta(&ctx->happiness, FEED_HAPPINESS_BOOST);
    apply_delta(&ctx->energy, FEED_ENERGY_BOOST);
    ctx->last_feed_ms = now_ms;
    ctx->last_interaction_ms = now_ms;
    ctx->sleep_forced = false;
    ctx->angry_until_ms = 0;
    ctx->dizzy_burst_window_start_ms = 0;
    ctx->dizzy_burst_count = 0;

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

void emotion_on_pet(emotion_context_t *ctx, uint32_t intensity, uint32_t now_ms) {
    if (!ctx) {
        return;
    }
    // intensity: 0=small, 1=big
    int32_t happy = PET_HAPPINESS_BOOST;
    int32_t social = PET_SOCIAL_BOOST;
    if (intensity > 0) {
        happy += 2;
        social += 5;
    }
    apply_delta(&ctx->happiness, happy);
    apply_delta(&ctx->social, social);
    apply_delta(&ctx->fear, -PET_FEAR_DROP);
    if (intensity > 0) {
        apply_delta(&ctx->eldritch_charge, PET_ELDRITCH_BIG_BOOST);
    }
    ctx->last_pet_ms = now_ms;
    ctx->last_interaction_ms = now_ms;
    ctx->sleep_forced = false;
    ctx->angry_until_ms = 0;
    ctx->dizzy_burst_window_start_ms = 0;
    ctx->dizzy_burst_count = 0;

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
    apply_delta_hunger(&ctx->hunger, PLAY_HUNGER_RISE);
    apply_delta(&ctx->eldritch_charge, PLAY_ELDRITCH_BOOST);
    ctx->last_play_ms = now_ms;
    ctx->last_interaction_ms = now_ms;
    ctx->sleep_forced = false;
    ctx->angry_until_ms = 0;
    ctx->dizzy_burst_window_start_ms = 0;
    ctx->dizzy_burst_count = 0;

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
    ctx->last_interaction_ms = now_ms;
    ctx->sleep_forced = false;

    if (ctx->dizzy_burst_window_start_ms == 0 ||
        elapsed_ms(ctx->dizzy_burst_window_start_ms, now_ms) > s_angry_dizzy_window_ms) {
        ctx->dizzy_burst_window_start_ms = now_ms;
        ctx->dizzy_burst_count = 1;
    } else if (ctx->dizzy_burst_count < 255U) {
        ctx->dizzy_burst_count++;
    }

    if (ctx->dizzy_burst_count > s_angry_dizzy_count_threshold) {
        uint32_t hold_ms = random_between_u32(s_angry_override_min_ms, s_angry_override_max_ms);
        uint32_t until = now_ms + hold_ms;
        if (!timer_active(now_ms, ctx->angry_until_ms) ||
            ((int32_t)(until - ctx->angry_until_ms) > 0)) {
            ctx->angry_until_ms = until;
        }
        apply_delta(&ctx->happiness, -3);
        ctx->dizzy_burst_window_start_ms = now_ms;
        ctx->dizzy_burst_count = 0;
        log_event(LOG_LEVEL_INFO, TAG,
                  "Angry escalation: dizzy burst exceeded threshold (%u), hold=%ums",
                  (unsigned)s_angry_dizzy_count_threshold, (unsigned)hold_ms);
    }

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
    ctx->last_interaction_ms = now_ms;
    ctx->sleep_forced = false;

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
    ctx->last_interaction_ms = now_ms;
    ctx->sleep_forced = false;

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
    ctx->last_interaction_ms = now_ms;
    ctx->sleep_forced = false;
    ctx->angry_until_ms = 0;
    ctx->dizzy_burst_window_start_ms = 0;
    ctx->dizzy_burst_count = 0;

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

emotion_state_t emotion_get_state(const emotion_context_t *ctx) {
    if (!ctx) {
        return EMOTION_STATE_NEUTRAL;
    }
    return ctx->current_state;
}

emotion_need_mask_t emotion_get_needs(const emotion_context_t *ctx) {
    if (!ctx) {
        return EMO_NEED_NONE;
    }
    return ctx->active_needs;
}

emotion_affect_t emotion_get_affect(const emotion_context_t *ctx, int8_t *valence_out, int8_t *arousal_out) {
    if (!ctx) {
        if (valence_out) *valence_out = 0;
        if (arousal_out) *arousal_out = 0;
        return EMO_AFFECT_NEUTRAL;
    }
    return compute_affect(ctx, valence_out, arousal_out);
}

void emotion_set_affect_weights(const emotion_affect_weights_t *weights)
{
    if (!weights) {
        return;
    }
    s_affect_weights.happiness_pct = clamp_weight_pct(weights->happiness_pct);
    s_affect_weights.satiety_pct = clamp_weight_pct(weights->satiety_pct);
    s_affect_weights.energy_pct = clamp_weight_pct(weights->energy_pct);
    s_affect_weights.social_pct = clamp_weight_pct(weights->social_pct);
    s_affect_weights.fear_pct = clamp_weight_pct(weights->fear_pct);
    EL_LOGI(TAG, "Affect weights set: happy=%u satiety=%u energy=%u social=%u fear=%u",
            (unsigned)s_affect_weights.happiness_pct,
            (unsigned)s_affect_weights.satiety_pct,
            (unsigned)s_affect_weights.energy_pct,
            (unsigned)s_affect_weights.social_pct,
            (unsigned)s_affect_weights.fear_pct);
}

void emotion_get_affect_weights(emotion_affect_weights_t *out_weights)
{
    if (!out_weights) {
        return;
    }
    *out_weights = s_affect_weights;
}

void emotion_set_sleep_window(uint8_t start_hour, uint8_t end_hour) {
    if (start_hour < 24) s_sleep_start_hour = start_hour;
    if (end_hour < 24) s_sleep_end_hour = end_hour;
    EL_LOGI(TAG, "Sleep window set start=%u end=%u", (unsigned)s_sleep_start_hour, (unsigned)s_sleep_end_hour);
}

void emotion_force_sleep(emotion_context_t *ctx, bool enable, uint32_t now_ms) {
    if (!ctx) return;
    ctx->sleep_forced = enable;
    ctx->last_interaction_ms = now_ms;
    if (enable) {
        apply_delta(&ctx->energy, -5);
        log_event(LOG_LEVEL_INFO, TAG, "Sleep forced ON at %ums", (unsigned)now_ms);
    } else {
        log_event(LOG_LEVEL_INFO, TAG, "Sleep forced OFF at %ums", (unsigned)now_ms);
    }
}

void emotion_set_mood_log_interval_minutes(uint32_t minutes) {
    if (minutes == 0) {
        s_mood_log_interval_ms = 0;
    } else {
        s_mood_log_interval_ms = minutes * 60U * 1000U;
    }
    EL_LOGI(TAG, "Mood log interval set to %u minutes%s", (unsigned)minutes, minutes == 0 ? " (disabled)" : "");
}

uint32_t emotion_get_mood_log_interval_ms(void)
{
    return s_mood_log_interval_ms;
}

void emotion_set_angry_policy(uint8_t dizzy_count_threshold,
                              uint32_t dizzy_window_ms,
                              uint32_t override_min_ms,
                              uint32_t override_max_ms)
{
    if (dizzy_count_threshold < 1U) dizzy_count_threshold = 1U;
    if (dizzy_window_ms < 10000U) dizzy_window_ms = 10000U;
    if (override_min_ms < 1000U) override_min_ms = 1000U;
    if (override_max_ms < override_min_ms) override_max_ms = override_min_ms;
    if (override_max_ms > 30U * 60U * 1000U) override_max_ms = 30U * 60U * 1000U;

    s_angry_dizzy_count_threshold = dizzy_count_threshold;
    s_angry_dizzy_window_ms = dizzy_window_ms;
    s_angry_override_min_ms = override_min_ms;
    s_angry_override_max_ms = override_max_ms;

    EL_LOGI(TAG, "Angry policy set: dizzy_count>%u window=%ums hold=%u..%ums",
            (unsigned)s_angry_dizzy_count_threshold,
            (unsigned)s_angry_dizzy_window_ms,
            (unsigned)s_angry_override_min_ms,
            (unsigned)s_angry_override_max_ms);
}

void emotion_on_curiosity_ping(emotion_context_t *ctx, uint32_t now_ms) {
    if (!ctx) return;
    // Placeholder: small positive nudge, can be tuned later when curiosity sensors arrive.
    apply_delta(&ctx->happiness, 1);
    apply_delta(&ctx->social, 1);
    ctx->last_interaction_ms = now_ms;
    ctx->sleep_forced = false;
    ctx->angry_until_ms = 0;
    ctx->dizzy_burst_window_start_ms = 0;
    ctx->dizzy_burst_count = 0;
}

void emotion_on_sensor_irritation(emotion_context_t *ctx, uint8_t intensity, uint32_t duration_ms, uint32_t now_ms)
{
    if (!ctx) {
        return;
    }
    uint32_t dur = duration_ms;
    if (dur < 1000U) dur = 1000U;
    if (dur > 30U * 60U * 1000U) dur = 30U * 60U * 1000U;

    int32_t bounded = (int32_t)intensity;
    if (bounded > 100) bounded = 100;

    // Small negative-valence nudge without turning into scared by default.
    apply_delta(&ctx->happiness, -(bounded / 20)); // up to -5
    apply_delta(&ctx->fear, +(bounded / 25));      // up to +4

    uint32_t until = now_ms + dur;
    if (!timer_active(now_ms, ctx->angry_until_ms) ||
        ((int32_t)(until - ctx->angry_until_ms) > 0)) {
        ctx->angry_until_ms = until;
    }
    ctx->last_interaction_ms = now_ms;
    ctx->sleep_forced = false;

    emotion_state_t next = emotion_select_state(ctx, now_ms);
    handle_state_transition(ctx, next, now_ms);
}

__attribute__((weak)) void emotion_notify_state_change(emotion_state_t old_state,
                                                       emotion_state_t new_state) {
    log_event(LOG_LEVEL_INFO, TAG, "State changed %s -> %s", state_name(old_state), state_name(new_state));
}
