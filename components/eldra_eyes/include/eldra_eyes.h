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

typedef struct eldra_eyes_context eldra_eyes_context_t;

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

/**
 * @brief Toggle a test pattern (border + crosshair) to validate centering/edges.
 */
void eldra_eyes_set_test_pattern(bool enable);

/**
 * @brief Set a global display center offset (applies to target/current centers).
 */
void eldra_eyes_set_display_center_offset(int x_offset, int y_offset);

#endif /* ELDRA_EYES_H */
