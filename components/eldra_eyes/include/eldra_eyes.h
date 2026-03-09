#ifndef ELDRA_EYES_H
#define ELDRA_EYES_H

/**
 * @file eldra_eyes.h
 * @brief Public API for the Eldra eye animation engine (stubbed for now).
 *
 * The engine manages modes (CHIBI vs. ELDRITCH), moods, and transformation
 * triggers driven by the cowl reed switch and future sensor/rune events.
 * Rendering and clip selection are defined in docs/eldra_eyes_animation.md.
 */
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ELDRA_EYES_MODE_CHIBI = 0,
    ELDRA_EYES_MODE_ELDRITCH = 1,
} eldra_eyes_mode_t;

typedef enum {
    ELDRA_EYES_MOOD_NEUTRAL = 0,
    ELDRA_EYES_MOOD_HAPPY,
    ELDRA_EYES_MOOD_SAD,
    ELDRA_EYES_MOOD_ANGRY,
    ELDRA_EYES_MOOD_SLEEPY,
    ELDRA_EYES_MOOD_HUNGRY,
    ELDRA_EYES_MOOD_BORED,
    ELDRA_EYES_MOOD_ELDRITCH_RUNE,
} eldra_eyes_mood_t;

typedef enum {
    ELDRA_EYES_MOD_NONE    = 0,
    ELDRA_EYES_MOD_SLEEPY  = 1 << 0,
    ELDRA_EYES_MOD_HUNGRY  = 1 << 1,
    ELDRA_EYES_MOD_LONELY  = 1 << 2,
    ELDRA_EYES_MOD_SCARED  = 1 << 3,
    ELDRA_EYES_MOD_PLAYFUL = 1 << 4,
} eldra_eyes_modifier_t;

typedef struct eldra_eyes_context eldra_eyes_context_t;

typedef struct {
    bool blink_active;
    bool look_active;
    bool dizzy_active;
    bool idle_clip_active;
} eldra_eyes_activity_t;

typedef struct {
    float gyro_thresh_dps;
    float gyro_spike_dps;
    float gdev_thresh;
    uint32_t accum_ms;
    uint32_t cooldown_ms;
} eldra_eyes_dizzy_config_t;

/**
 * @brief Allocate and initialize a new Eldra eyes context.
 *
 * The new context starts in CHIBI mode and NEUTRAL mood,
 * with no active transformations.
 *
 * @return Pointer to a newly allocated context, or NULL on failure.
 */
eldra_eyes_context_t *eldra_eyes_create(void);

/**
 * @brief Destroy a previously created Eldra eyes context.
 *
 * Safe to call with NULL.
 *
 * @param ctx Pointer to the context to destroy.
 */
void eldra_eyes_destroy(eldra_eyes_context_t *ctx);

/**
 * @brief Set the current eye mode (CHIBI or ELDRITCH).
 *
 * This does not automatically trigger any transform animation.
 * It is a low-level override that forces the mode.
 */
void eldra_eyes_set_mode(eldra_eyes_context_t *ctx, eldra_eyes_mode_t mode);

/**
 * @brief Get the current eye mode.
 */
eldra_eyes_mode_t eldra_eyes_get_mode(const eldra_eyes_context_t *ctx);

/**
 * @brief Set the current mood.
 */
void eldra_eyes_set_mood(eldra_eyes_context_t *ctx, eldra_eyes_mood_t mood);

/**
 * @brief Get the current mood.
 */
eldra_eyes_mood_t eldra_eyes_get_mood(const eldra_eyes_context_t *ctx);

/**
 * @brief Set concurrent eye mood modifiers (bitmask of @ref eldra_eyes_modifier_t).
 */
void eldra_eyes_set_modifiers(eldra_eyes_context_t *ctx, uint32_t modifier_mask);

/**
 * @brief Get current modifier bitmask.
 */
uint32_t eldra_eyes_get_modifiers(const eldra_eyes_context_t *ctx);

/**
 * @brief Control sleepy lid intensity profile.
 *
 * false = more-open tired look (default sleepy)
 * true  = heavier droop (used near actual sleep transition)
 */
void eldra_eyes_set_sleep_lid_heavy(eldra_eyes_context_t *ctx, bool heavy);

/**
 * @brief Set base lid depths used by sleepy/angry overlays.
 *
 * Values are clamped to a safe visual range [0..6].
 */
void eldra_eyes_set_lid_depths(uint8_t sleep_depth, uint8_t angry_depth);

/**
 * @brief Get current base lid depths for sleepy/angry overlays.
 */
void eldra_eyes_get_lid_depths(uint8_t *sleep_depth, uint8_t *angry_depth);

/**
 * @brief Configure IMU thresholds used to trigger reactive dizzy.
 *
 * Values are clamped to safe ranges. This affects runtime behavior immediately.
 */
void eldra_eyes_set_dizzy_config(const eldra_eyes_dizzy_config_t *cfg);

/**
 * @brief Read current IMU dizzy trigger thresholds.
 */
void eldra_eyes_get_dizzy_config(eldra_eyes_dizzy_config_t *out_cfg);

/**
 * @brief Trigger a CHIBI -> ELDRITCH transform animation.
 *
 * This will mark an internal transform state as active and reset its timer.
 * The actual mode switch will be handled later in the update logic.
 */
void eldra_eyes_trigger_transform_chibi_to_eldritch(eldra_eyes_context_t *ctx);

/**
 * @brief Trigger an ELDRITCH -> CHIBI transform animation.
 */
void eldra_eyes_trigger_transform_eldritch_to_chibi(eldra_eyes_context_t *ctx);

/**
 * @brief Trigger a temporary dizzy/spiral reactive animation.
 *
 * @param ctx Eyes context.
 * @param duration_ms Duration in milliseconds (set 0 for default ~2200 ms).
 */
void eldra_eyes_trigger_dizzy(eldra_eyes_context_t *ctx, uint32_t duration_ms);

/**
 * @brief Feed IMU samples (gyro in dps, accel in g) to drive reactive animations/logging.
 *
 * Typical use: call every IMU sample with dt_ms since last call.
 */
void eldra_eyes_handle_imu(eldra_eyes_context_t *ctx,
                           float gx_dps,
                           float gy_dps,
                           float gz_dps,
                           float ax_g,
                           float ay_g,
                           float az_g,
                           uint32_t dt_ms);

/**
 * @brief Advance internal animation and transform state.
 *
 * @param ctx    Eyes context.
 * @param dt_ms  Time step in milliseconds since last update.
 */
void eldra_eyes_update(eldra_eyes_context_t *ctx, uint32_t dt_ms);

/**
 * @brief Render the eyes into a framebuffer.
 *
 * For now, this will be a stub. Later it will draw into a 16-bit RGB565 framebuffer
 * that is ultimately pushed to the 480x480 round display.
 *
 * @param ctx         Eyes context.
 * @param framebuffer Pointer to the start of the framebuffer.
 * @param fb_width    Framebuffer width in pixels.
 * @param fb_height   Framebuffer height in pixels.
 */
void eldra_eyes_render(eldra_eyes_context_t *ctx,
                       uint16_t *framebuffer,
                       uint16_t fb_width,
                       uint16_t fb_height);

/**
 * @brief Override the render center offsets (pixels).
 *
 * Positive x moves right, positive y moves down. Useful for runtime tuning
 * without rebuilding.
 */
void eldra_eyes_set_center_offset(int x_offset, int y_offset);
void eldra_eyes_get_center_offset(int *x_offset, int *y_offset);

/**
 * @brief Toggle a test pattern (border + crosshair) to validate centering/edges.
 */
void eldra_eyes_set_test_pattern(bool enable);

/**
 * @brief Set a global display center offset (applies to target/current centers).
 */
void eldra_eyes_set_display_center_offset(int x_offset, int y_offset);
void eldra_eyes_get_display_center_offset(int *x_offset, int *y_offset);

/**
 * @brief Get the currently effective center offset after runtime clamping.
 *
 * This reflects what render uses on the 480x480 panel after accounting for
 * display center offset and eye sprite bounds.
 */
void eldra_eyes_get_effective_center_offset(int *x_offset, int *y_offset);

/**
 * @brief Get the effective eye-center offset used on the most recent render.
 *
 * This is the post-clamp eye offset (relative to display center) from the
 * last call to @ref eldra_eyes_render, and is intended for layers that must
 * stay visually locked to the eyes (e.g., glyph overlays).
 */
void eldra_eyes_get_last_render_center_offset(int *x_offset, int *y_offset);

/**
 * @brief Snapshot current animation activity flags.
 */
void eldra_eyes_get_activity(const eldra_eyes_context_t *ctx, eldra_eyes_activity_t *out);

#endif /* ELDRA_EYES_H */
