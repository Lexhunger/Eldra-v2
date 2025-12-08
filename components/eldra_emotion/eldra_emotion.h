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
} emotion_state_t;

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

    bool hood_closed;

    bool dizzy_active;
    uint32_t dizzy_until_ms;

    bool scared_active;
    uint32_t scared_until_ms;
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
 * @brief Event: feeding Eldra.
 * @param ctx Emotion context.
 * @param food_type Application-defined food identifier.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_feed(emotion_context_t *ctx, uint32_t food_type, uint32_t now_ms);

/**
 * @brief Event: being petted/touched.
 * @param ctx Emotion context.
 * @param now_ms Current monotonic time in milliseconds.
 */
void emotion_on_pet(emotion_context_t *ctx, uint32_t now_ms);

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
 * @brief Retrieve the current emotional state.
 * @param ctx Emotion context.
 * @return Active emotion_state_t.
 */
emotion_state_t emotion_get_state(const emotion_context_t *ctx);

/**
 * @brief Weak hook invoked whenever the state changes. Higher layers override to drive eyes/haptics.
 *        Default implementation logs the transition and does nothing else.
 * @param old_state Previous state.
 * @param new_state New state after selection.
 */
void emotion_notify_state_change(emotion_state_t old_state, emotion_state_t new_state);
