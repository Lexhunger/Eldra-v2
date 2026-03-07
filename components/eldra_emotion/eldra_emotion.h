#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @file eldra_emotion.h
 * @brief Public API for Eldra's Emotion Engine. Hardware-agnostic: consumes events and emits state changes.
 */

/**
 * @brief Named emotional states selected from meter values and priority rules.
 */
typedef enum {
    EMOTION_STATE_NEUTRAL = 0,
    EMOTION_STATE_HAPPY,
    EMOTION_STATE_SAD,
    EMOTION_STATE_LONELY,
    EMOTION_STATE_SLEEPY,
    EMOTION_STATE_HUNGRY,
    EMOTION_STATE_PLAYFUL,
    EMOTION_STATE_ELDRITCH,
    EMOTION_STATE_SCARED,
    EMOTION_STATE_DIZZY,
    EMOTION_STATE_ANGRY,
} emotion_state_t;

/**
 * @brief Derived affect tone (for expression blending).
 */
typedef enum {
    EMO_AFFECT_NEUTRAL = 0,
    EMO_AFFECT_HAPPY,
    EMO_AFFECT_SAD,
    EMO_AFFECT_ANGRY,
    EMO_AFFECT_EXCITED,
} emotion_affect_t;

/**
 * @brief Runtime affect weighting (percent; 100 = default behavior).
 * These weights influence derived valence/arousal and affect classification.
 */
typedef struct {
    uint16_t happiness_pct;
    uint16_t satiety_pct;
    uint16_t energy_pct;
    uint16_t social_pct;
    uint16_t fear_pct;
} emotion_affect_weights_t;

/**
 * @brief Bitmask of concurrent needs/modifiers so renderers can blend (not just pick one state).
 */
typedef enum {
    EMO_NEED_NONE           = 0,
    EMO_NEED_HUNGRY         = 1 << 0,
    EMO_NEED_LONELY         = 1 << 1,
    EMO_NEED_SLEEPY         = 1 << 2,
    EMO_NEED_SCARED         = 1 << 3,
    EMO_NEED_PLAYFUL        = 1 << 4,
    EMO_NEED_ELDRITCH_READY = 1 << 5, // allowed only when not hungry/sleepy
} emotion_need_mask_t;

/**
 * @brief Runtime context for the Emotion Engine. All meters are bounded in [0, 100].
 */
typedef struct {
    uint8_t happiness;
    uint8_t hunger;
    uint8_t energy;
    uint8_t social;
    uint8_t fear;
    uint8_t eldritch_charge;
    uint8_t battery_percent;

    emotion_state_t current_state;
    emotion_state_t previous_state;
    uint32_t last_state_change_ms;
    uint32_t last_tick_ms;
    uint32_t tick_accum_ms;

    uint32_t last_play_ms;
    uint32_t last_feed_ms;
    uint32_t last_pet_ms;
    uint32_t last_shake_ms;
    uint32_t last_voice_ms;
    uint32_t last_edge_ms;
    uint32_t eldritch_accum_ms;
    uint32_t last_interaction_ms;
    uint32_t last_mood_log_ms;
    uint32_t dizzy_burst_window_start_ms;
    uint8_t dizzy_burst_count;

    bool hood_closed;

    bool dizzy_active;
    uint32_t dizzy_until_ms;

    bool scared_active;
    uint32_t scared_until_ms;
    uint32_t angry_until_ms;

    emotion_need_mask_t active_needs;
    emotion_affect_t affect;
    bool sleep_forced;
} emotion_context_t;

/**
 * @brief Initialize the Emotion Engine with baseline meter values.
 * @param ctx Pointer to context storage.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_init(emotion_context_t *ctx, uint32_t now_ms);

/**
 * @brief Periodic maintenance tick: apply passive meter decay/growth and refresh timers.
 * @param ctx Emotion context.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_tick(emotion_context_t *ctx, uint32_t now_ms);

/**
 * @brief Update the tracked battery percentage (0-100). Callers compute the percentage from hardware readings.
 * @param ctx Emotion context.
 * @param percent Battery level percent (clamped to 0-100).
 */
void emotion_set_battery_percent(emotion_context_t *ctx, uint8_t percent);

/**
 * @brief Event: feeding Eldra.
 * @param ctx Emotion context.
 * @param food_type Application-defined food identifier.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_feed(emotion_context_t *ctx, uint32_t food_type, uint32_t now_ms);

/**
 * @brief Optional future hook: curiosity/explore stimulus.
 */
void emotion_on_curiosity_ping(emotion_context_t *ctx, uint32_t now_ms);

/**
 * @brief Event: being petted/touched.
 * @param ctx Emotion context.
 * @param intensity 0=small, 1=big pet (larger social boost)
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_pet(emotion_context_t *ctx, uint32_t intensity, uint32_t now_ms);

/**
 * @brief Event: play interaction.
 * @param ctx Emotion context.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_play(emotion_context_t *ctx, uint32_t now_ms);

/**
 * @brief Event: shaken or spun.
 * @param ctx Emotion context.
 * @param intensity Arbitrary intensity unit from motion detection.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_shake(emotion_context_t *ctx, int intensity, uint32_t now_ms);

/**
 * @brief Event: edge/ledge detected (e.g., ToF drop-off).
 * @param ctx Emotion context.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_edge_detected(emotion_context_t *ctx, uint32_t now_ms);

/**
 * @brief Event: hood/cowl state changed.
 * @param ctx Emotion context.
 * @param hood_closed True if the hood is currently closed.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_hood_state_changed(emotion_context_t *ctx, bool hood_closed, uint32_t now_ms);

/**
 * @brief Event: a recognized voice command or phrase.
 * @param ctx Emotion context.
 * @param phrase_id Application-defined phrase identifier.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_voice_command(emotion_context_t *ctx, uint32_t phrase_id, uint32_t now_ms);

/**
 * @brief Sensor hook: trigger a temporary angry window from non-social stimuli
 *        (e.g., repeated bumps, rough motion, noisy environment, etc.).
 * @param ctx Emotion context.
 * @param intensity 0..100 rough irritation magnitude.
 * @param duration_ms How long to hold anger-priority before normal evaluation resumes.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_sensor_irritation(emotion_context_t *ctx, uint8_t intensity, uint32_t duration_ms, uint32_t now_ms);

/**
 * @brief Retrieve the current emotional state.
 * @param ctx Emotion context.
 * @return Active emotion_state_t.
 */
emotion_state_t emotion_get_state(const emotion_context_t *ctx);

/**
 * @brief Retrieve the current need/mood bitmask for blending.
 */
emotion_need_mask_t emotion_get_needs(const emotion_context_t *ctx);

/**
 * @brief Retrieve derived affect (neutral/happy/sad/angry/excited).
 * @param ctx Emotion context.
 * @param valence_out Optional: signed valence score (-100..100).
 * @param arousal_out Optional: signed arousal score (-100..100).
 */
emotion_affect_t emotion_get_affect(const emotion_context_t *ctx, int8_t *valence_out, int8_t *arousal_out);
void emotion_set_affect_weights(const emotion_affect_weights_t *weights);
void emotion_get_affect_weights(emotion_affect_weights_t *out_weights);

/**
 * @brief Configure quiet-hour sleep window (start>=0,end<=23).
 */
void emotion_set_sleep_window(uint8_t start_hour, uint8_t end_hour);
void emotion_force_sleep(emotion_context_t *ctx, bool enable, uint32_t now_ms);

/**
 * @brief Configure mood log interval in minutes (0 disables periodic logs).
 */
void emotion_set_mood_log_interval_minutes(uint32_t minutes);
uint32_t emotion_get_mood_log_interval_ms(void);

/**
 * @brief Configure angry escalation policy.
 * @param dizzy_count_threshold Trigger angry override when dizzy count in window exceeds this value.
 * @param dizzy_window_ms Rolling window for dizzy counting.
 * @param override_min_ms Minimum timed angry hold (ms) when escalation triggers.
 * @param override_max_ms Maximum timed angry hold (ms) when escalation triggers.
 */
void emotion_set_angry_policy(uint8_t dizzy_count_threshold,
                              uint32_t dizzy_window_ms,
                              uint32_t override_min_ms,
                              uint32_t override_max_ms);

/**
 * @brief Weak hook invoked whenever the state changes. Higher layers override to drive eyes/haptics.
 *        Default implementation logs the transition and does nothing else.
 * @param old_state Previous state.
 * @param new_state New state after selection.
 */
void emotion_notify_state_change(emotion_state_t old_state, emotion_state_t new_state);
