#include "eldra_eyes.h"

/**
 * @file eldra_eyes.c
 * @brief Eye engine stub with idle animations (breath + weighted blink/look).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_random.h"
#include "eldra_logging.h"

static const char *TAG = "eldra_eyes";

// Small manual offsets to fine-tune placement on the round LCD. Defaults to centered.
#ifndef ELDRA_EYES_CENTER_X_OFFSET
#define ELDRA_EYES_CENTER_X_OFFSET 0
#endif
#ifndef ELDRA_EYES_CENTER_Y_OFFSET
#define ELDRA_EYES_CENTER_Y_OFFSET 0
#endif
static int s_center_x_offset = ELDRA_EYES_CENTER_X_OFFSET;
static int s_center_y_offset = ELDRA_EYES_CENTER_Y_OFFSET;

static const int k_breath_period_ms = 1500;
static const int k_breath_scale_amp = 1; // reduced amplitude for subtler motion
static const int k_breath_offset_amp_px = 3;
static const float k_breath_phase_offset_left = 0.35f; // ~20 degrees
static const float k_two_pi = 6.2831853f;
static const float k_rad_to_deg = 57.2957795f;
static const float k_imu_ema_alpha = 0.18f;
static const float k_imu_gyro_dizzy_thresh_dps = 180.0f;
static const uint32_t k_imu_gyro_dizzy_time_ms = 400;
static const uint32_t k_dizzy_cooldown_ms = 4000;
static const uint32_t k_imu_log_interval_ms = 1500;

static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Idle clip identifiers */
typedef enum {
    IDLE_CLIP_NONE = 0,
    IDLE_CLIP_BLINK = 1,
    IDLE_CLIP_LOOK = 2,
    IDLE_CLIP_DOUBLE_BLINK = 3,
    IDLE_CLIP_EXTENDED_BLINK = 4,
} idle_clip_t;

typedef struct {
    idle_clip_t clip;
    int weight;            /* relative selection weight */
    uint32_t min_duration; /* ms */
    uint32_t max_duration; /* ms */
    uint32_t min_gap;      /* ms between plays */
} idle_entry_t;

/**
 * @brief Internal state for the Eldra eyes engine.
 */
struct eldra_eyes_context {
    eldra_eyes_mode_t mode;
    eldra_eyes_mood_t mood;

    bool transform_chibi_to_eldritch_active;
    bool transform_eldritch_to_chibi_active;
    uint32_t transform_timer_ms;

    /* Idle animation state */
    uint32_t breath_phase_ms;
    struct {
        bool active;
        uint32_t elapsed_ms;
        uint32_t duration_ms;
        uint32_t gap_ms;
        uint8_t repeat_count;
        uint32_t hold_ms;
    } blink;
    struct {
        bool active;
        int direction; /* -1 left, +1 right */
        uint32_t elapsed_ms;
        uint32_t duration_ms;
        uint32_t frame_elapsed_ms;
        size_t frame_index;
    } look;
    struct {
        idle_clip_t active_clip; /* current clip; 0 = none */
        idle_clip_t last_clip;
        uint32_t clip_elapsed_ms;
        uint32_t clip_duration_ms;
        uint32_t idle_gap_ms;
    } idle;

    struct {
        bool active;
        uint32_t elapsed_ms;
        uint32_t duration_ms;
    } reactive_dizzy;
    struct {
        float gyro_ema_dps;
        float accel_ema_g;
        uint32_t above_thresh_ms;
        uint32_t cooldown_ms;
        uint32_t log_timer_ms;
    } imu;

    int base_scale; /* baseline scale for chibi eyes */
};

/* Idle clip table (weights drive random selection) */
static const idle_entry_t k_idle_entries[] = {
    {IDLE_CLIP_BLINK, 10, 140, 200, 700},          // more frequent single blinks
    {IDLE_CLIP_LOOK,  8, 500, 900, 900},           // still frequent looks
    {IDLE_CLIP_DOUBLE_BLINK, 1, 140, 200, 40000},  // rare double-blink (~1/min)
    {IDLE_CLIP_EXTENDED_BLINK, 1, 200, 220, 120000}, // rare long closed-eye rest (few minutes)
};

/* --- CHIBI SPRITE DATA (PALETTE-INDEXED) ---------------------------------- */

// 8-color RGB565 palette corresponding to the design reference.
static const uint16_t k_palette_rgb565[8] = {
    0x0000, // #000000
    0x0004, // #000220
    0x0089, // #00134A
    0x01B1, // #00358C
    0x0356, // #0068B3
    0x0C3A, // #0887D2
    0x0C19, // #0D82CA
    0x663E, // #67C4F1
};

// 11x11 eye sprites using palette indices (mirrored for left/right in render).
static const uint8_t k_eye_frame_center[11][11] = {
    {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 2, 3, 3, 3, 2, 1, 0, 0},
    {0, 1, 2, 3, 4, 4, 4, 3, 2, 1, 0},
    {0, 2, 3, 6, 2, 1, 2, 5, 3, 2, 1},
    {1, 3, 4, 2, 3, 0, 7, 2, 6, 3, 1},
    {1, 3, 4, 3, 1, 0, 0, 3, 6, 3, 1},
    {1, 3, 6, 3, 1, 0, 0, 3, 6, 3, 1},
    {1, 3, 4, 4, 3, 1, 3, 4, 4, 3, 1},
    {0, 2, 3, 5, 5, 4, 6, 4, 3, 2, 0},
    {0, 0, 1, 2, 3, 3, 3, 2, 1, 0, 0},
    {0, 0, 0, 1, 1, 2, 1, 1, 0, 0, 0},
};

// Look right (mid shift)
static const uint8_t k_eye_frame_right_mid[11][11] = {
    {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 1, 2, 3, 3, 3, 2, 0, 0},
    {0, 1, 1, 2, 3, 4, 4, 4, 3, 2, 0},
    {0, 1, 2, 3, 6, 2, 1, 2, 5, 3, 2},
    {1, 1, 3, 4, 2, 3, 1, 7, 2, 6, 3},
    {1, 1, 3, 4, 3, 0, 0, 1, 3, 6, 3},
    {1, 1, 3, 6, 3, 0, 0, 1, 3, 6, 3},
    {1, 1, 3, 4, 4, 3, 1, 3, 4, 4, 3},
    {0, 1, 2, 3, 5, 5, 4, 6, 4, 3, 2},
    {0, 0, 1, 1, 2, 3, 3, 3, 2, 0, 0},
    {0, 0, 0, 1, 1, 1, 2, 1, 0, 0, 0},
};

// Look right (full shift)
static const uint8_t k_eye_frame_right_full[11][11] = {
    {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 1, 1, 2, 3, 3, 3, 2, 0},
    {0, 1, 1, 1, 2, 3, 4, 4, 4, 3, 2},
    {0, 1, 1, 2, 3, 6, 2, 1, 2, 5, 3},
    {1, 1, 1, 3, 4, 2, 3, 1, 7, 2, 6},
    {1, 1, 1, 3, 4, 3, 0, 1, 1, 3, 6},
    {1, 1, 1, 3, 6, 3, 0, 1, 1, 3, 6},
    {1, 1, 1, 3, 4, 4, 3, 1, 3, 4, 4},
    {0, 1, 1, 2, 3, 5, 5, 4, 6, 4, 3},
    {0, 0, 1, 1, 1, 2, 3, 3, 3, 2, 0},
    {0, 0, 0, 1, 1, 1, 1, 2, 0, 0, 0},
};

// Look left (mid shift)
static const uint8_t k_eye_frame_left_mid[11][11] = {
    {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 2, 3, 3, 3, 2, 1, 1, 0, 0},
    {0, 2, 3, 4, 4, 4, 3, 2, 1, 1, 0},
    {2, 3, 6, 2, 1, 2, 5, 3, 2, 1, 1},
    {3, 4, 2, 3, 1, 7, 2, 6, 3, 1, 1},
    {3, 4, 3, 1, 1, 0, 3, 6, 3, 1, 1},
    {3, 6, 3, 1, 1, 0, 3, 6, 3, 1, 1},
    {3, 4, 4, 3, 1, 3, 4, 4, 3, 1, 1},
    {2, 3, 5, 5, 4, 6, 4, 3, 2, 1, 0},
    {0, 0, 2, 3, 3, 3, 2, 1, 1, 0, 0},
    {0, 0, 0, 1, 2, 1, 1, 1, 0, 0, 0},
};

// Look left (full shift)
static const uint8_t k_eye_frame_left_full[11][11] = {
    {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0},
    {0, 2, 3, 3, 3, 2, 1, 1, 1, 0, 0},
    {2, 3, 4, 4, 4, 3, 2, 1, 1, 1, 0},
    {3, 6, 2, 1, 2, 5, 3, 2, 1, 1, 1},
    {4, 2, 3, 1, 7, 2, 6, 3, 1, 1, 1},
    {4, 3, 1, 1, 1, 3, 6, 3, 1, 1, 1},
    {6, 3, 1, 1, 1, 3, 6, 3, 1, 1, 1},
    {4, 4, 3, 1, 3, 4, 4, 3, 1, 1, 1},
    {3, 5, 5, 4, 6, 4, 3, 2, 1, 1, 0},
    {0, 2, 3, 3, 3, 2, 1, 1, 1, 0, 0},
    {0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0},
};

typedef struct {
    const uint8_t (*frame)[11];
    uint16_t duration_ms;
} eye_frame_step_t;

static const eye_frame_step_t k_look_right_sequence[] = {
    {k_eye_frame_center, 120},
    {k_eye_frame_right_mid, 160},
    {k_eye_frame_right_mid, 160},  // linger in mid for smoother ease-in
    {k_eye_frame_right_full, 160},
    {k_eye_frame_right_full, 1000}, // extra hold at the side
    {k_eye_frame_right_mid, 160},
    {k_eye_frame_center, 220},
};

static const eye_frame_step_t k_look_left_sequence[] = {
    {k_eye_frame_center, 120},
    {k_eye_frame_left_mid, 160},
    {k_eye_frame_left_mid, 160},   // linger in mid for smoother ease-in
    {k_eye_frame_left_full, 160},
    {k_eye_frame_left_full, 1000}, // extra hold at the side
    {k_eye_frame_left_mid, 160},
    {k_eye_frame_center, 220},
};

static const size_t k_look_right_sequence_len = sizeof(k_look_right_sequence) / sizeof(k_look_right_sequence[0]);
static const size_t k_look_left_sequence_len = sizeof(k_look_left_sequence) / sizeof(k_look_left_sequence[0]);

static uint32_t look_sequence_total_ms(const eye_frame_step_t *seq, size_t len)
{
    uint32_t total = 0;
    for (size_t i = 0; i < len; ++i) {
        total += seq[i].duration_ms;
    }
    return total;
}

static uint32_t blink_total_ms(uint32_t duration_ms, uint32_t hold_ms, uint32_t gap_ms, uint8_t repeats)
{
    if (repeats == 0) {
        return 0;
    }
    uint32_t close_ms = duration_ms / 2;
    uint32_t open_ms = duration_ms - close_ms;
    uint32_t cycle_ms = close_ms + hold_ms + open_ms + gap_ms;
    return cycle_ms * (uint32_t)repeats;
}

static const char *idle_clip_name(idle_clip_t clip)
{
    switch (clip) {
    case IDLE_CLIP_BLINK: return "BLINK";
    case IDLE_CLIP_DOUBLE_BLINK: return "DOUBLE_BLINK";
    case IDLE_CLIP_EXTENDED_BLINK: return "EXTENDED_BLINK";
    case IDLE_CLIP_LOOK: return "LOOK";
    default: return "NONE";
    }
}

static const uint8_t (*look_sequence_frame(const struct eldra_eyes_context *ctx))[11]
{
    const eye_frame_step_t *seq = (ctx->look.direction >= 0) ? k_look_right_sequence : k_look_left_sequence;
    size_t seq_len = (ctx->look.direction >= 0) ? k_look_right_sequence_len : k_look_left_sequence_len;
    if (ctx->look.active && ctx->look.frame_index < seq_len) {
        return seq[ctx->look.frame_index].frame;
    }
    return k_eye_frame_center;
}

static void advance_look(struct eldra_eyes_context *ctx, uint32_t dt_ms)
{
    if (!ctx->look.active) {
        return;
    }

    const eye_frame_step_t *seq = (ctx->look.direction >= 0) ? k_look_right_sequence : k_look_left_sequence;
    size_t seq_len = (ctx->look.direction >= 0) ? k_look_right_sequence_len : k_look_left_sequence_len;

    ctx->look.elapsed_ms += dt_ms;
    ctx->look.frame_elapsed_ms += dt_ms;

    while (ctx->look.frame_index < seq_len &&
           ctx->look.frame_elapsed_ms >= seq[ctx->look.frame_index].duration_ms) {
        ctx->look.frame_elapsed_ms -= seq[ctx->look.frame_index].duration_ms;
        ctx->look.frame_index++;
    }

    if (ctx->look.elapsed_ms >= ctx->look.duration_ms || ctx->look.frame_index >= seq_len) {
        ctx->look.active = false;
        ctx->look.elapsed_ms = 0;
        ctx->look.frame_elapsed_ms = 0;
        ctx->look.frame_index = 0;
    }
}

static void fill_background(uint16_t *framebuffer, uint16_t fb_width, uint16_t fb_height, uint16_t color)
{
    if (!framebuffer) {
        return;
    }
    const size_t total = (size_t)fb_width * (size_t)fb_height;
    for (size_t i = 0; i < total; ++i) {
        framebuffer[i] = color;
    }
}

static void blit_eye(uint16_t *framebuffer,
                     uint16_t fb_width,
                     uint16_t fb_height,
                     int x0,
                     int y0,
                     int scale_x,
                     int scale_y,
                     const uint8_t frame_indices[11][11])
{
    if (!framebuffer || scale_x <= 0 || scale_y <= 0) {
        return;
    }

    for (int row = 0; row < 11; ++row) {
        for (int col = 0; col < 11; ++col) {
            uint16_t color = k_palette_rgb565[frame_indices[row][col]];
            int px = x0 + col * scale_x;
            int py = y0 + row * scale_y;
            for (int dy = 0; dy < scale_y; ++dy) {
                int y = py + dy;
                if (y < 0 || y >= fb_height) {
                    continue;
                }
                size_t row_offset = (size_t)y * fb_width;
                for (int dx = 0; dx < scale_x; ++dx) {
                    int x = px + dx;
                    if (x < 0 || x >= fb_width) {
                        continue;
                    }
                    framebuffer[row_offset + x] = color;
                }
            }
        }
    }
}

static void blit_eye_closed(uint16_t *framebuffer,
                            uint16_t fb_width,
                            uint16_t fb_height,
                            int x0,
                            int y0,
                            int scale_x,
                            int scale_y)
{
    uint16_t color = k_palette_rgb565[1]; // eyelid color
    int width = 11 * scale_x;
    int height = 11 * scale_y;
    for (int row = 0; row < height; ++row) {
        int y = y0 + row;
        if (y < 0 || y >= fb_height) {
            continue;
        }
        size_t row_offset = (size_t)y * fb_width;
        for (int col = 0; col < width; ++col) {
            int x = x0 + col;
            if (x < 0 || x >= fb_width) {
                continue;
            }
            framebuffer[row_offset + x] = color;
        }
    }
}

static void blit_eye_spiral(uint16_t *framebuffer,
                            uint16_t fb_width,
                            uint16_t fb_height,
                            int x0,
                            int y0,
                            int scale_x,
                            int scale_y,
                            float phase)
{
    if (!framebuffer || scale_x <= 0 || scale_y <= 0) {
        return;
    }
    for (int row = 0; row < 11; ++row) {
        for (int col = 0; col < 11; ++col) {
            float dx = (float)col - 5.0f;
            float dy = (float)row - 5.0f;
            float r = sqrtf(dx * dx + dy * dy);
            float ang = atan2f(dy, dx) + phase;
            float wrap = fmodf(ang - (r * 0.85f), k_two_pi);
            if (wrap < 0.0f) {
                wrap += k_two_pi;
            }
            uint16_t color = k_palette_rgb565[0];
            if (r <= 5.5f) {
                if (wrap < 0.35f) {
                    color = k_palette_rgb565[7];
                } else if (wrap < 0.65f) {
                    color = k_palette_rgb565[5];
                } else if (r < 2.3f) {
                    color = k_palette_rgb565[4];
                }
            }
            int px = x0 + col * scale_x;
            int py = y0 + row * scale_y;
            for (int dy_scale = 0; dy_scale < scale_y; ++dy_scale) {
                int y = py + dy_scale;
                if (y < 0 || y >= fb_height) {
                    continue;
                }
                size_t row_offset = (size_t)y * fb_width;
                for (int dx_scale = 0; dx_scale < scale_x; ++dx_scale) {
                    int x = px + dx_scale;
                    if (x < 0 || x >= fb_width) {
                        continue;
                    }
                    framebuffer[row_offset + x] = color;
                }
            }
        }
    }
}

/* Pick the next idle clip, weighted and avoiding immediate repeats (except breath which is continuous). */
static void idle_pick_next(struct eldra_eyes_context *ctx)
{
    int total_weight = 0;
    for (size_t i = 0; i < sizeof(k_idle_entries) / sizeof(k_idle_entries[0]); ++i) {
        total_weight += k_idle_entries[i].weight;
    }

    idle_clip_t chosen = IDLE_CLIP_NONE;
    for (int attempt = 0; attempt < 4 && chosen == IDLE_CLIP_NONE; ++attempt) {
        int r = (int)(esp_random() % (uint32_t)total_weight);
        for (size_t i = 0; i < sizeof(k_idle_entries) / sizeof(k_idle_entries[0]); ++i) {
            const idle_entry_t *e = &k_idle_entries[i];
            if (r < e->weight) {
                if (e->clip != ctx->idle.last_clip || attempt == 3) {
                    chosen = e->clip;
                }
                break;
            }
            r -= e->weight;
        }
    }

    ctx->idle.active_clip = chosen;
    ctx->idle.clip_elapsed_ms = 0;
    if (chosen == IDLE_CLIP_BLINK) {
        ctx->blink.active = true;
        ctx->blink.elapsed_ms = 0;
        ctx->blink.duration_ms = 140; // faster single blink
        ctx->blink.gap_ms = 0;
        ctx->blink.repeat_count = 1;
        ctx->blink.hold_ms = 0;
        ctx->idle.clip_duration_ms = blink_total_ms(ctx->blink.duration_ms,
                                                    ctx->blink.hold_ms,
                                                    ctx->blink.gap_ms,
                                                    ctx->blink.repeat_count);
        EL_LOGD(TAG, "Idle start: %s (duration=%u)", idle_clip_name(chosen), ctx->idle.clip_duration_ms);
    } else if (chosen == IDLE_CLIP_LOOK) {
        ctx->look.active = true;
        ctx->look.elapsed_ms = 0;
        ctx->look.direction = (esp_random() & 0x01) ? 1 : -1;
        ctx->look.frame_elapsed_ms = 0;
        ctx->look.frame_index = 0;
        ctx->look.duration_ms = (ctx->look.direction >= 0)
            ? look_sequence_total_ms(k_look_right_sequence, k_look_right_sequence_len)
            : look_sequence_total_ms(k_look_left_sequence, k_look_left_sequence_len);
        ctx->idle.clip_duration_ms = ctx->look.duration_ms;
        EL_LOGD(TAG, "Idle start: %s dir=%s (duration=%u)", idle_clip_name(chosen),
                 (ctx->look.direction >= 0) ? "RIGHT" : "LEFT", ctx->idle.clip_duration_ms);
    } else if (chosen == IDLE_CLIP_DOUBLE_BLINK) {
        ctx->blink.active = true;
        ctx->blink.elapsed_ms = 0;
        ctx->blink.duration_ms = 130; // faster double blink
        ctx->blink.gap_ms = 90;
        ctx->blink.repeat_count = 2;
        ctx->blink.hold_ms = 0;
        ctx->idle.clip_duration_ms = blink_total_ms(ctx->blink.duration_ms,
                                                    ctx->blink.hold_ms,
                                                    ctx->blink.gap_ms,
                                                    ctx->blink.repeat_count);
        EL_LOGD(TAG, "Idle start: %s (duration=%u)", idle_clip_name(chosen), ctx->idle.clip_duration_ms);
    } else if (chosen == IDLE_CLIP_EXTENDED_BLINK) {
        ctx->blink.active = true;
        ctx->blink.elapsed_ms = 0;
        ctx->blink.duration_ms = 200;
        ctx->blink.gap_ms = 0;
        ctx->blink.repeat_count = 1;
        ctx->blink.hold_ms = 1000 + (esp_random() % 1201); // 1.0s to 2.2s hold closed
        ctx->idle.clip_duration_ms = blink_total_ms(ctx->blink.duration_ms,
                                                    ctx->blink.hold_ms,
                                                    ctx->blink.gap_ms,
                                                    ctx->blink.repeat_count);
        EL_LOGD(TAG, "Idle start: %s (duration=%u hold=%u)", idle_clip_name(chosen),
                 ctx->idle.clip_duration_ms, ctx->blink.hold_ms);
    } else {
        ctx->idle.clip_duration_ms = 0;
    }
}

eldra_eyes_context_t *eldra_eyes_create(void)
{
    eldra_eyes_context_t *ctx = (eldra_eyes_context_t *)calloc(1, sizeof(eldra_eyes_context_t));
    if (!ctx) {
        return NULL;
    }

    ctx->base_scale = 14;
    ctx->mode = ELDRA_EYES_MODE_CHIBI;
    ctx->mood = ELDRA_EYES_MOOD_NEUTRAL;
    ctx->transform_chibi_to_eldritch_active = false;
    ctx->transform_eldritch_to_chibi_active = false;
    ctx->transform_timer_ms = 0;

    ctx->breath_phase_ms = 0;
    ctx->blink.active = false;
    ctx->blink.elapsed_ms = 0;
    ctx->blink.duration_ms = 180;
    ctx->blink.gap_ms = 0;
    ctx->blink.repeat_count = 1;
    ctx->blink.hold_ms = 0;
    ctx->look.active = false;
    ctx->look.elapsed_ms = 0;
    ctx->look.duration_ms = 0;
    ctx->look.direction = 1;
    ctx->look.frame_elapsed_ms = 0;
    ctx->look.frame_index = 0;
    ctx->idle.active_clip = IDLE_CLIP_NONE;
    ctx->idle.last_clip = IDLE_CLIP_NONE;
    ctx->idle.clip_elapsed_ms = 0;
    ctx->idle.clip_duration_ms = 0;
    ctx->idle.idle_gap_ms = 500; // initial gap before first idle clip
    ctx->reactive_dizzy.active = false;
    ctx->reactive_dizzy.elapsed_ms = 0;
    ctx->reactive_dizzy.duration_ms = 0;
    ctx->imu.gyro_ema_dps = 0.0f;
    ctx->imu.accel_ema_g = 0.0f;
    ctx->imu.above_thresh_ms = 0;
    ctx->imu.cooldown_ms = 0;
    ctx->imu.log_timer_ms = 0;
    return ctx;
}

void eldra_eyes_destroy(eldra_eyes_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    free(ctx);
}

void eldra_eyes_set_center_offset(int x_offset, int y_offset)
{
    s_center_x_offset = x_offset;
    s_center_y_offset = y_offset;
    EL_LOGI(TAG, "Eyes center offset set to x=%d y=%d", s_center_x_offset, s_center_y_offset);
}

void eldra_eyes_set_mode(eldra_eyes_context_t *ctx, eldra_eyes_mode_t mode)
{
    if (!ctx) {
        return;
    }
    ctx->mode = mode;
}

eldra_eyes_mode_t eldra_eyes_get_mode(const eldra_eyes_context_t *ctx)
{
    if (!ctx) {
        return ELDRA_EYES_MODE_CHIBI;
    }
    return ctx->mode;
}

void eldra_eyes_set_mood(eldra_eyes_context_t *ctx, eldra_eyes_mood_t mood)
{
    if (!ctx) {
        return;
    }
    ctx->mood = mood;
}

eldra_eyes_mood_t eldra_eyes_get_mood(const eldra_eyes_context_t *ctx)
{
    if (!ctx) {
        return ELDRA_EYES_MOOD_NEUTRAL;
    }
    return ctx->mood;
}

void eldra_eyes_trigger_transform_chibi_to_eldritch(eldra_eyes_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    ctx->transform_chibi_to_eldritch_active = true;
    ctx->transform_eldritch_to_chibi_active = false;
    ctx->transform_timer_ms = 0;
}

void eldra_eyes_trigger_transform_eldritch_to_chibi(eldra_eyes_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    ctx->transform_chibi_to_eldritch_active = false;
    ctx->transform_eldritch_to_chibi_active = true;
    ctx->transform_timer_ms = 0;
}

void eldra_eyes_trigger_dizzy(eldra_eyes_context_t *ctx, uint32_t duration_ms)
{
    if (!ctx) {
        return;
    }
    ctx->reactive_dizzy.active = true;
    ctx->reactive_dizzy.elapsed_ms = 0;
    ctx->reactive_dizzy.duration_ms = (duration_ms == 0) ? 2200 : duration_ms;
    EL_LOGI(TAG, "Reactive: DIZZY start (duration=%u)", ctx->reactive_dizzy.duration_ms);
}

void eldra_eyes_handle_imu(eldra_eyes_context_t *ctx,
                           float gx_dps,
                           float gy_dps,
                           float gz_dps,
                           float ax_g,
                           float ay_g,
                           float az_g,
                           uint32_t dt_ms)
{
    if (!ctx) {
        return;
    }
    if (dt_ms == 0) {
        dt_ms = 1;
    }

    float gyro_mag = sqrtf(gx_dps * gx_dps + gy_dps * gy_dps + gz_dps * gz_dps);
    float accel_mag = sqrtf(ax_g * ax_g + ay_g * ay_g + az_g * az_g);

    /* EMA smoothing */
    float alpha = k_imu_ema_alpha;
    ctx->imu.gyro_ema_dps = (1.0f - alpha) * ctx->imu.gyro_ema_dps + alpha * gyro_mag;
    ctx->imu.accel_ema_g = (1.0f - alpha) * ctx->imu.accel_ema_g + alpha * accel_mag;

    if (ctx->imu.cooldown_ms > 0) {
        if (ctx->imu.cooldown_ms > dt_ms) {
            ctx->imu.cooldown_ms -= dt_ms;
        } else {
            ctx->imu.cooldown_ms = 0;
        }
    }

    /* Trigger dizzy if sustained rotation and not already in a reactive state */
    if (!ctx->reactive_dizzy.active && ctx->imu.cooldown_ms == 0) {
        if (gyro_mag > k_imu_gyro_dizzy_thresh_dps) {
            ctx->imu.above_thresh_ms += dt_ms;
            if (ctx->imu.above_thresh_ms >= k_imu_gyro_dizzy_time_ms) {
                eldra_eyes_trigger_dizzy(ctx, 0);
                ctx->imu.above_thresh_ms = 0;
                ctx->imu.cooldown_ms = k_dizzy_cooldown_ms;
                EL_LOGD(TAG, "IMU: Dizzy trigger (gyro=%.1f dps)", (double)gyro_mag);
            }
        } else {
            ctx->imu.above_thresh_ms = 0;
        }
    }

    /* Whip detection removed */

    /* Periodic debug log */
    if (ctx->imu.log_timer_ms > dt_ms) {
        ctx->imu.log_timer_ms -= dt_ms;
    } else {
        float roll_deg = atan2f(ay_g, az_g) * k_rad_to_deg;
        float pitch_deg = atan2f(-ax_g, sqrtf(ay_g * ay_g + az_g * az_g)) * k_rad_to_deg;
        EL_LOGD(TAG, "IMU: gyro=%.1f dps (ema=%.1f) accel=%.2f g (ema=%.2f) roll=%.1f pitch=%.1f thresh=%0.1fms/%0.1fdps cool=%ums",
                 (double)gyro_mag,
                 (double)ctx->imu.gyro_ema_dps,
                 (double)accel_mag,
                 (double)ctx->imu.accel_ema_g,
                 (double)roll_deg,
                 (double)pitch_deg,
                 (double)k_imu_gyro_dizzy_time_ms,
                 (double)k_imu_gyro_dizzy_thresh_dps,
                 (unsigned)ctx->imu.cooldown_ms);
        ctx->imu.log_timer_ms = k_imu_log_interval_ms;
    }
}

void eldra_eyes_update(eldra_eyes_context_t *ctx, uint32_t dt_ms)
{
    if (!ctx) {
        return;
    }

    if (ctx->transform_chibi_to_eldritch_active || ctx->transform_eldritch_to_chibi_active) {
        ctx->transform_timer_ms += dt_ms;
    }

    /* Reactive dizzy timer */
    if (ctx->reactive_dizzy.active) {
        ctx->reactive_dizzy.elapsed_ms += dt_ms;
        if (ctx->reactive_dizzy.elapsed_ms >= ctx->reactive_dizzy.duration_ms) {
            ctx->reactive_dizzy.active = false;
            ctx->reactive_dizzy.elapsed_ms = 0;
            ctx->reactive_dizzy.duration_ms = 0;
            ctx->idle.idle_gap_ms = 600 + (esp_random() % 400); // settle before next idle
            EL_LOGI(TAG, "Reactive: DIZZY end");
        }
    }

    /* Breath runs continuously */
    ctx->breath_phase_ms = (ctx->breath_phase_ms + dt_ms) % k_breath_period_ms;

    /* Idle clip manager for blink/look */
    if (ctx->idle.active_clip == IDLE_CLIP_NONE && !ctx->reactive_dizzy.active) {
        if (ctx->idle.idle_gap_ms > dt_ms) {
            ctx->idle.idle_gap_ms -= dt_ms;
        } else {
            idle_pick_next(ctx);
        }
    } else {
        ctx->idle.clip_elapsed_ms += dt_ms;
        if (ctx->idle.clip_elapsed_ms >= ctx->idle.clip_duration_ms) {
            ctx->idle.last_clip = ctx->idle.active_clip;
            EL_LOGD(TAG, "Idle end: %s", idle_clip_name(ctx->idle.last_clip));
            ctx->idle.active_clip = IDLE_CLIP_NONE;
            ctx->idle.clip_elapsed_ms = 0;
            ctx->idle.clip_duration_ms = 0;
            /* Set a gap before the next idle clip; blinks rest longer */
            if (ctx->idle.last_clip == IDLE_CLIP_BLINK ||
                ctx->idle.last_clip == IDLE_CLIP_DOUBLE_BLINK ||
                ctx->idle.last_clip == IDLE_CLIP_EXTENDED_BLINK) {
                if (ctx->idle.last_clip == IDLE_CLIP_EXTENDED_BLINK) {
                    ctx->idle.idle_gap_ms = 90000 + (esp_random() % 60000); // 1.5-2.5 minutes
                } else if (ctx->idle.last_clip == IDLE_CLIP_DOUBLE_BLINK) {
                    ctx->idle.idle_gap_ms = 30000 + (esp_random() % 20000); // 30-50 seconds
                } else {
                    ctx->idle.idle_gap_ms = 600 + (esp_random() % 400); // 0.6-1.0s
                }
            } else {
                ctx->idle.idle_gap_ms = 600 + (esp_random() % 400); // 0.6-1.0s between looks
            }
            /* End any clip-specific state */
            ctx->blink.active = false;
            ctx->look.active = false;
            ctx->look.elapsed_ms = 0;
            ctx->look.frame_elapsed_ms = 0;
            ctx->look.frame_index = 0;
        }
    }

    /* Advance blink state if active */
    if (ctx->blink.active) {
        ctx->blink.elapsed_ms += dt_ms;
        uint32_t total_ms = blink_total_ms(ctx->blink.duration_ms,
                                           ctx->blink.hold_ms,
                                           ctx->blink.gap_ms,
                                           ctx->blink.repeat_count);
        if (ctx->blink.elapsed_ms >= total_ms) {
            ctx->blink.active = false;
            ctx->blink.elapsed_ms = 0;
        }
    }

    /* Advance look state if active */
    if (ctx->look.active) {
        advance_look(ctx, dt_ms);
    }
}

void eldra_eyes_render(eldra_eyes_context_t *ctx,
                       uint16_t *framebuffer,
                       uint16_t fb_width,
                       uint16_t fb_height)
{
    if (!ctx || !framebuffer || fb_width == 0 || fb_height == 0) {
        return;
    }

    // Fill background with palette index 0.
    fill_background(framebuffer, fb_width, fb_height, k_palette_rgb565[0]);

    /* Breath effect:
     *  - Smooth sinusoidal wave instead of triangular
     *  - Drives slight vertical scale change
     *  - Also moves eyes up/down a few pixels
     *  - Small phase offset between left/right eyes to feel more organic
     */
    int scale_x = ctx->base_scale; // keep width steady
    int scale_y_base = ctx->base_scale;

    bool reactive_dizzy = ctx->reactive_dizzy.active;
    bool breath_enabled = (ctx->idle.active_clip == IDLE_CLIP_NONE) && !reactive_dizzy;
    float breath_angle = breath_enabled ? (k_two_pi * ((float)ctx->breath_phase_ms / (float)k_breath_period_ms)) : 0.0f;
    float breath_wave = breath_enabled ? sinf(breath_angle) : 0.0f;
    float left_eye_wave = breath_enabled ? sinf(breath_angle + k_breath_phase_offset_left) : 0.0f;

    int breath_delta_right = breath_enabled ? (int)(breath_wave * (float)k_breath_scale_amp) : 0;
    int breath_delta_left = breath_enabled ? (int)(left_eye_wave * (float)k_breath_scale_amp) : 0;
    int breath_offset_right_px = breath_enabled ? (int)(breath_wave * (float)k_breath_offset_amp_px) : 0;
    int breath_offset_left_px = breath_enabled ? (int)(left_eye_wave * (float)k_breath_offset_amp_px) : 0;

    int scale_y_left = scale_y_base + breath_delta_left;
    int scale_y_right = scale_y_base + breath_delta_right;
    if (scale_y_left < 1) scale_y_left = 1;
    if (scale_y_right < 1) scale_y_right = 1;

    const uint8_t (*eye_frame)[11] = look_sequence_frame(ctx);
    const int sprite_px_x = 11 * scale_x;
    const int sprite_px_y_nominal = 11 * scale_y_base;
    const int center_y = (fb_height / 2) + s_center_y_offset;
    const int center_x = (fb_width / 2) + s_center_x_offset;
    const int eye_offset = (sprite_px_x * 3) / 4; // Horizontal offset from center

    const int base_y0 = center_y - (sprite_px_y_nominal / 2);
    int left_y0 = base_y0 + breath_offset_left_px;
    int right_y0 = base_y0 + breath_offset_right_px;

    bool blink_cover = false;
    if (ctx->blink.active && !reactive_dizzy) {
        uint32_t close_ms = ctx->blink.duration_ms / 2;
        uint32_t open_ms = ctx->blink.duration_ms - close_ms;
        uint32_t cycle_ms = close_ms + ctx->blink.hold_ms + open_ms + ctx->blink.gap_ms;
        uint32_t total_ms = blink_total_ms(ctx->blink.duration_ms,
                                           ctx->blink.hold_ms,
                                           ctx->blink.gap_ms,
                                           ctx->blink.repeat_count);
        if (cycle_ms > 0 && ctx->blink.elapsed_ms < total_ms) {
            uint32_t cycle_index = ctx->blink.elapsed_ms / cycle_ms;
            if (cycle_index < ctx->blink.repeat_count) {
                uint32_t t_in_cycle = ctx->blink.elapsed_ms % cycle_ms;
                int min_scale = (ctx->base_scale / 4);
                if (min_scale < 1) {
                    min_scale = 1;
                }
                int scale_range = ctx->base_scale - min_scale;
                if (t_in_cycle < close_ms) {
                    int span = (close_ms == 0) ? 1 : (int)close_ms;
                    int scale_adjust = (scale_range * (int)t_in_cycle) / span;
                    int s = ctx->base_scale - scale_adjust;
                    if (ctx->blink.hold_ms > 0) {
                        int extended_min = ctx->base_scale / 3;
                        if (extended_min < 2) {
                            extended_min = 2;
                        }
                        if (s < extended_min) {
                            s = extended_min;
                        }
                    }
                    scale_y_left = s;
                    scale_y_right = s;
                } else if (t_in_cycle < close_ms + ctx->blink.hold_ms) {
                    blink_cover = (ctx->blink.hold_ms > 0);
                    if (!blink_cover) {
                        scale_y_left = min_scale;
                        scale_y_right = min_scale;
                    } else {
                        int s = ctx->base_scale / 3;
                        if (s < 2) {
                            s = 2;
                        }
                        scale_y_left = s;
                        scale_y_right = s;
                    }
                } else if (t_in_cycle < close_ms + ctx->blink.hold_ms + open_ms) {
                    uint32_t t_open = t_in_cycle - (close_ms + ctx->blink.hold_ms);
                    int span = (open_ms == 0) ? 1 : (int)open_ms;
                    int scale_adjust = (scale_range * (int)t_open) / span;
                    int s = min_scale + scale_adjust;
                    if (ctx->blink.hold_ms > 0) {
                        int extended_min = ctx->base_scale / 3;
                        if (extended_min < 2) {
                            extended_min = 2;
                        }
                        if (s < extended_min) {
                            s = extended_min;
                        }
                    }
                    scale_y_left = s;
                    scale_y_right = s;
                }
            }
        }
    }

    if (scale_y_left < 1) scale_y_left = 1;
    if (scale_y_right < 1) scale_y_right = 1;

    int left_x0 = (center_x - eye_offset) - (sprite_px_x / 2);
    int right_x0 = (center_x + eye_offset) - (sprite_px_x / 2);

    // Clamp positions to framebuffer bounds so we never wrap or draw outside.
    const int min_x = 0;
    const int max_x = fb_width - sprite_px_x;
    const int min_y = 0;
    const int max_y = fb_height - sprite_px_y_nominal;
    left_x0 = clamp_int(left_x0, min_x, max_x);
    right_x0 = clamp_int(right_x0, min_x, max_x);
    left_y0 = clamp_int(left_y0, min_y, max_y);
    right_y0 = clamp_int(right_y0, min_y, max_y);

    if (reactive_dizzy) {
        float spin_phase = ((float)ctx->reactive_dizzy.elapsed_ms / 1000.0f) * (k_two_pi * 2.5f); // ~2.5 turns per second
        blit_eye_spiral(framebuffer, fb_width, fb_height,
                        left_x0, left_y0, scale_x, scale_y_left, spin_phase);
        blit_eye_spiral(framebuffer, fb_width, fb_height,
                        right_x0, right_y0, scale_x, scale_y_right, spin_phase + 0.4f);
    } else if (blink_cover) {
        int lid_offset_left = scale_y_left / 2;
        int lid_offset_right = scale_y_right / 2;
        blit_eye_closed(framebuffer, fb_width, fb_height,
                        left_x0, left_y0 + lid_offset_left, scale_x, scale_y_left);
        blit_eye_closed(framebuffer, fb_width, fb_height,
                        right_x0, right_y0 + lid_offset_right, scale_x, scale_y_right);
    } else {
        blit_eye(framebuffer, fb_width, fb_height,
                 left_x0, left_y0, scale_x, scale_y_left, eye_frame);
        blit_eye(framebuffer, fb_width, fb_height,
                 right_x0, right_y0, scale_x, scale_y_right, eye_frame);
    }
}

