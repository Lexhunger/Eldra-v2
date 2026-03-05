#include "eldra_eyes.h"

/**
 * @file eldra_eyes.c
 * @brief Eye engine stub with idle animations (breath + weighted blink/look).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
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
static int s_display_center_x_offset = 0;
static int s_display_center_y_offset = 0;
static int s_last_effective_eye_x_offset = 0;
static int s_last_effective_eye_y_offset = 0;
static int s_shadow_center_x_offset = ELDRA_EYES_CENTER_X_OFFSET;
static int s_shadow_center_y_offset = ELDRA_EYES_CENTER_Y_OFFSET;
static int s_shadow_display_center_x_offset = 0;
static int s_shadow_display_center_y_offset = 0;
static portMUX_TYPE s_offset_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_sleep_lid_depth = 2;
static uint8_t s_angry_lid_depth = 2;

static const int k_breath_period_ms = 1500;
static const int k_breath_scale_amp = 1; // reduced amplitude for subtler motion
static const int k_breath_offset_amp_px = 3;
static const float k_breath_phase_offset_left = 0.35f; // ~20 degrees
static const float k_two_pi = 6.2831853f;
static const float k_rad_to_deg = 57.2957795f;
static const float k_imu_ema_alpha = 0.18f;
// Keep dizzy easy to trigger with real shake motion while avoiding tiny jitter.
static const float k_imu_gyro_dizzy_thresh_dps = 45.0f;
static const float k_imu_gyro_dizzy_spike_dps = 95.0f;
static const float k_imu_gyro_decay_floor_dps = 22.0f;
static const float k_imu_gdev_dizzy_thresh = 0.22f;
static const uint32_t k_imu_gyro_dizzy_time_ms = 110;
static const uint32_t k_dizzy_cooldown_ms = 3000;
static const uint32_t k_imu_log_interval_ms = 1500;
static const uint32_t k_idle_force_blink_ms = 4500;
static const uint32_t k_idle_force_look_ms = 9000;
static bool s_test_pattern = false;

static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void copy_eye_frame(uint8_t dst[11][11], const uint8_t src[11][11]);
static void build_tired_frame(const uint8_t src[11][11], uint8_t dst[11][11], uint8_t lid_depth, bool heavy);
static void apply_hungry_tint(const uint8_t src[11][11], uint8_t dst[11][11]);

static void compute_effective_offsets(uint16_t fb_width,
                                      uint16_t fb_height,
                                      int sprite_px_x,
                                      int sprite_px_y_nominal,
                                      int req_center_x,
                                      int req_center_y,
                                      int disp_center_x,
                                      int disp_center_y,
                                      int *out_x,
                                      int *out_y)
{
    const int eye_offset = (sprite_px_x * 3) / 4;
    const int base_center_x = (fb_width / 2) + disp_center_x;
    const int min_center_x = eye_offset + (sprite_px_x / 2);
    const int max_center_x = (int)fb_width - eye_offset - (sprite_px_x / 2);
    int min_x_offset = min_center_x - base_center_x;
    int max_x_offset = max_center_x - base_center_x;
    if (min_x_offset > max_x_offset) {
        int mid = (min_x_offset + max_x_offset) / 2;
        min_x_offset = mid;
        max_x_offset = mid;
    }

    // Include breathing motion (+/-3px) and max per-eye vertical scale swing (~11px).
    const int vertical_margin = k_breath_offset_amp_px + 12;
    const int base_center_y = (fb_height / 2) + disp_center_y;
    const int min_center_y = (sprite_px_y_nominal / 2) + vertical_margin;
    const int max_center_y = (int)fb_height - (sprite_px_y_nominal / 2) - vertical_margin;
    int min_y_offset = min_center_y - base_center_y;
    int max_y_offset = max_center_y - base_center_y;
    if (min_y_offset > max_y_offset) {
        int mid = (min_y_offset + max_y_offset) / 2;
        min_y_offset = mid;
        max_y_offset = mid;
    }

    if (out_x) {
        *out_x = clamp_int(req_center_x, min_x_offset, max_x_offset);
    }
    if (out_y) {
        *out_y = clamp_int(req_center_y, min_y_offset, max_y_offset);
    }
}

static void snapshot_offsets(int *center_x, int *center_y, int *disp_x, int *disp_y)
{
    bool healed = false;
    int bad_cx = 0, bad_cy = 0, bad_dx = 0, bad_dy = 0;
    int healed_cx = 0, healed_cy = 0, healed_dx = 0, healed_dy = 0;

    portENTER_CRITICAL(&s_offset_mux);
    if (s_center_x_offset != s_shadow_center_x_offset ||
        s_center_y_offset != s_shadow_center_y_offset ||
        s_display_center_x_offset != s_shadow_display_center_x_offset ||
        s_display_center_y_offset != s_shadow_display_center_y_offset) {
        healed = true;
        bad_cx = s_center_x_offset;
        bad_cy = s_center_y_offset;
        bad_dx = s_display_center_x_offset;
        bad_dy = s_display_center_y_offset;
        s_center_x_offset = s_shadow_center_x_offset;
        s_center_y_offset = s_shadow_center_y_offset;
        s_display_center_x_offset = s_shadow_display_center_x_offset;
        s_display_center_y_offset = s_shadow_display_center_y_offset;
    }
    healed_cx = s_center_x_offset;
    healed_cy = s_center_y_offset;
    healed_dx = s_display_center_x_offset;
    healed_dy = s_display_center_y_offset;
    portEXIT_CRITICAL(&s_offset_mux);

    if (healed) {
        EL_LOGW(TAG,
                "Offset state healed at runtime bad eye=(%d,%d) disp=(%d,%d) -> eye=(%d,%d) disp=(%d,%d)",
                bad_cx, bad_cy, bad_dx, bad_dy,
                healed_cx, healed_cy, healed_dx, healed_dy);
    }

    if (center_x) *center_x = healed_cx;
    if (center_y) *center_y = healed_cy;
    if (disp_x) *disp_x = healed_dx;
    if (disp_y) *disp_y = healed_dy;
}

static void draw_square_marker(uint16_t *fb, uint16_t fb_width, uint16_t fb_height, int cx, int cy, uint16_t color)
{
    if (!fb || fb_width == 0 || fb_height == 0) return;
    for (int dy = -3; dy <= 3; ++dy) {
        int y = cy + dy;
        if (y < 0 || y >= (int)fb_height) continue;
        for (int dx = -3; dx <= 3; ++dx) {
            int x = cx + dx;
            if (x < 0 || x >= (int)fb_width) continue;
            fb[(size_t)y * fb_width + (uint16_t)x] = color;
        }
    }
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
    uint32_t modifiers;

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
        uint32_t since_blink_ms;
        uint32_t since_look_ms;
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
    bool sleep_lid_heavy;
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
/* Look frames with shifted highlight (v2) */
static const uint8_t k_eye_frame_right_mid_v2[11][11] = {
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,0,1,1,2,3,3,3,2,0,0},
    {0,1,1,2,3,4,4,4,3,2,0},
    {1,1,2,3,5,2,1,2,6,3,2},
    {1,1,3,6,2,3,1,3,7,4,3},
    {1,1,3,6,3,0,1,1,3,4,3},
    {1,1,3,6,3,0,1,1,3,6,3},
    {1,1,3,4,4,3,1,3,4,4,3},
    {0,1,2,3,4,6,4,5,5,3,2},
    {0,0,1,1,1,2,3,3,3,2,0},
    {0,0,0,1,1,1,1,2,1,0,0},
};

static const uint8_t k_eye_frame_right_full_v2[11][11] = {
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,0,1,1,1,2,3,3,3,2,0},
    {0,1,1,1,2,3,4,4,4,3,2},
    {1,1,1,2,3,5,2,1,2,6,3},
    {1,1,1,3,6,2,3,1,7,2,4},
    {1,1,1,3,6,3,1,1,1,3,4},
    {1,1,1,3,6,3,1,1,1,3,6},
    {1,1,1,3,4,4,3,1,3,4,4},
    {0,1,1,2,3,4,6,4,5,5,3},
    {0,0,1,1,1,2,3,3,3,2,1},
    {0,0,0,1,1,1,1,2,1,0,0},
};

static const uint8_t k_eye_frame_left_mid_v2[11][11] = {
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,0,2,3,3,3,2,1,1,0,0},
    {0,2,3,4,4,4,3,2,1,1,0},
    {2,3,6,2,1,2,5,3,2,1,1},
    {3,4,7,3,1,3,2,6,3,1,1},
    {3,4,3,1,1,0,3,6,3,1,1},
    {3,6,3,1,1,0,3,6,3,1,1},
    {3,4,4,3,1,3,4,4,3,1,1},
    {2,3,5,5,4,6,4,3,2,1,0},
    {0,2,3,3,3,2,1,1,1,0,0},
    {0,0,1,2,1,1,1,1,0,0,0},
};

static const uint8_t k_eye_frame_left_full_v2[11][11] = {
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,2,3,3,3,2,1,1,1,0,0},
    {2,3,4,4,4,3,2,1,1,1,0},
    {3,6,2,1,2,5,3,2,1,1,1},
    {4,7,3,1,3,2,6,3,1,1,1},
    {4,3,1,1,1,3,6,3,1,1,1},
    {6,3,1,1,1,3,6,3,1,1,1},
    {4,4,3,1,3,4,4,3,1,1,1},
    {3,5,5,4,6,4,3,2,1,1,0},
    {1,2,3,3,3,2,1,1,1,0,0},
    {0,0,1,2,1,1,1,1,0,0,0},
};
/* --- Blink frames (palette-indexed) --------------------------------------- */
static const uint8_t k_eye_frame_blink_mid[11][11] = {
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,0,0,2,3,3,3,2,0,0,0},
    {0,0,2,3,4,4,4,3,2,0,0},
    {0,1,3,4,6,6,6,4,3,1,0},
    {0,1,3,6,6,6,6,6,3,1,0},
    {0,1,3,6,6,6,6,6,3,1,0},
    {0,1,3,4,6,6,6,4,3,1,0},
    {0,0,2,3,4,4,4,3,2,0,0},
    {0,0,0,2,3,3,3,2,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
};

static const uint8_t k_eye_frame_blink_slim[11][11] = {
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,0,0,2,3,3,3,2,0,0,0},
    {0,0,0,2,6,6,6,2,0,0,0},
    {0,0,0,2,6,6,6,2,0,0,0},
    {0,0,0,2,3,3,3,2,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
};

static const uint8_t k_eye_frame_closed[11][11] = {
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0},
};

typedef struct {
    const uint8_t (*frame)[11];
    uint16_t duration_ms;
} eye_frame_step_t;

static const eye_frame_step_t k_look_right_sequence[] = {
    {k_eye_frame_center, 120},
    {k_eye_frame_right_mid_v2, 160},
    {k_eye_frame_right_mid_v2, 160},  // linger in mid for smoother ease-in
    {k_eye_frame_right_full_v2, 160},
    {k_eye_frame_right_full_v2, 1000}, // extra hold at the side
    {k_eye_frame_right_mid_v2, 160},
    {k_eye_frame_center, 220},
};

static const eye_frame_step_t k_look_left_sequence[] = {
    {k_eye_frame_center, 120},
    {k_eye_frame_left_mid_v2, 160},
    {k_eye_frame_left_mid_v2, 160},   // linger in mid for smoother ease-in
    {k_eye_frame_left_full_v2, 160},
    {k_eye_frame_left_full_v2, 1000}, // extra hold at the side
    {k_eye_frame_left_mid_v2, 160},
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
    const eye_frame_step_t *seq = k_look_right_sequence;
    size_t seq_len = k_look_right_sequence_len;
    if (ctx->look.direction < 0) {
        seq = k_look_left_sequence;
        seq_len = k_look_left_sequence_len;
    }
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

    const eye_frame_step_t *seq = k_look_right_sequence;
    size_t seq_len = k_look_right_sequence_len;
    if (ctx->look.direction < 0) {
        seq = k_look_left_sequence;
        seq_len = k_look_left_sequence_len;
    }

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

static void draw_test_pattern(uint16_t *fb, uint16_t fb_width, uint16_t fb_height, int cx, int cy)
{
    if (!fb || fb_width == 0 || fb_height == 0) return;
    const uint16_t bg = 0x0000;       // black
    const uint16_t border = 0x8410;   // faint gray border
    const uint16_t line_v = 0xF800;   // red vertical
    const uint16_t line_h = 0x07E0;   // green horizontal
    const uint16_t marker = 0xFFFF;   // white edge markers/center

    // Clear and border
    fill_background(fb, fb_width, fb_height, bg);
    for (uint16_t y = 0; y < fb_height; ++y) {
        for (uint16_t x = 0; x < fb_width; ++x) {
            bool is_edge = (x == 0) || (x == fb_width - 1) || (y == 0) || (y == fb_height - 1);
            if (is_edge) {
                fb[(size_t)y * fb_width + x] = border;
            }
        }
    }

    int ccx = clamp_int(cx, 0, (int)fb_width - 1);
    int ccy = clamp_int(cy, 0, (int)fb_height - 1);

    // Vertical red line (3px thick) that moves with X offset.
    for (int dx = -1; dx <= 1; ++dx) {
        int x = ccx + dx;
        if (x < 0 || x >= (int)fb_width) continue;
        for (uint16_t y = 0; y < fb_height; ++y) {
            fb[(size_t)y * fb_width + (uint16_t)x] = line_v;
        }
    }

    // Horizontal green line (3px thick) that moves with Y offset.
    for (int dy = -1; dy <= 1; ++dy) {
        int y = ccy + dy;
        if (y < 0 || y >= (int)fb_height) continue;
        for (uint16_t x = 0; x < fb_width; ++x) {
            fb[(size_t)y * fb_width + x] = line_h;
        }
    }

    // Edge markers at the ends of the lines so alignment is obvious.
    draw_square_marker(fb, fb_width, fb_height, ccx, 3, marker);                  // top edge on vertical
    draw_square_marker(fb, fb_width, fb_height, ccx, (int)fb_height - 4, marker); // bottom edge on vertical
    draw_square_marker(fb, fb_width, fb_height, 3, ccy, marker);                  // left edge on horizontal
    draw_square_marker(fb, fb_width, fb_height, (int)fb_width - 4, ccy, marker);  // right edge on horizontal

    // Center marker where lines intersect.
    draw_square_marker(fb, fb_width, fb_height, ccx, ccy, marker);
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
        ctx->idle.since_blink_ms = 0;
        EL_LOGD(TAG, "Idle start: %s (duration=%u)", idle_clip_name(chosen), ctx->idle.clip_duration_ms);
    } else if (chosen == IDLE_CLIP_LOOK) {
        ctx->look.active = true;
        ctx->look.elapsed_ms = 0;
        ctx->look.direction = (esp_random() & 0x01) ? 1 : -1;
        ctx->look.frame_elapsed_ms = 0;
        ctx->look.frame_index = 0;
        ctx->look.duration_ms = look_sequence_total_ms(k_look_right_sequence, k_look_right_sequence_len);
        ctx->idle.clip_duration_ms = ctx->look.duration_ms;
        ctx->idle.since_look_ms = 0;
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
        ctx->idle.since_blink_ms = 0;
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
        ctx->idle.since_blink_ms = 0;
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
    ctx->idle.since_blink_ms = 0;
    ctx->idle.since_look_ms = 0;
    ctx->reactive_dizzy.active = false;
    ctx->reactive_dizzy.elapsed_ms = 0;
    ctx->reactive_dizzy.duration_ms = 0;
    ctx->imu.gyro_ema_dps = 0.0f;
    ctx->imu.accel_ema_g = 0.0f;
    ctx->imu.above_thresh_ms = 0;
    ctx->imu.cooldown_ms = 0;
    ctx->imu.log_timer_ms = 0;
    ctx->sleep_lid_heavy = false;
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
    int disp_x = 0;
    int disp_y = 0;
    snapshot_offsets(NULL, NULL, &disp_x, &disp_y);

    portENTER_CRITICAL(&s_offset_mux);
    s_center_x_offset = x_offset;
    s_center_y_offset = y_offset;
    portEXIT_CRITICAL(&s_offset_mux);

    int clamped_x = x_offset;
    int clamped_y = y_offset;
    // Clamp immediately for the native 480x480 panel so persisted offsets
    // never store out-of-range values that can cause visual wrap artifacts.
    compute_effective_offsets(480, 480, 11 * 14, 11 * 14,
                              x_offset, y_offset, disp_x, disp_y,
                              &clamped_x, &clamped_y);

    portENTER_CRITICAL(&s_offset_mux);
    s_center_x_offset = clamped_x;
    s_center_y_offset = clamped_y;
    s_shadow_center_x_offset = clamped_x;
    s_shadow_center_y_offset = clamped_y;
    portEXIT_CRITICAL(&s_offset_mux);

    if (clamped_x != x_offset || clamped_y != y_offset) {
        EL_LOGW(TAG, "Eyes center offset clamped req=(%d,%d) -> (%d,%d)",
                x_offset, y_offset, clamped_x, clamped_y);
    } else {
        EL_LOGI(TAG, "Eyes center offset set to x=%d y=%d", s_center_x_offset, s_center_y_offset);
    }
}

void eldra_eyes_get_center_offset(int *x_offset, int *y_offset)
{
    int cx = 0, cy = 0;
    snapshot_offsets(&cx, &cy, NULL, NULL);
    if (x_offset) *x_offset = cx;
    if (y_offset) *y_offset = cy;
}

void eldra_eyes_set_test_pattern(bool enable)
{
    s_test_pattern = enable;
    EL_LOGI(TAG, "Eyes test pattern %s", enable ? "ENABLED" : "DISABLED");
}

void eldra_eyes_set_display_center_offset(int x_offset, int y_offset)
{
    portENTER_CRITICAL(&s_offset_mux);
    s_display_center_x_offset = x_offset;
    s_display_center_y_offset = y_offset;
    s_shadow_display_center_x_offset = x_offset;
    s_shadow_display_center_y_offset = y_offset;
    portEXIT_CRITICAL(&s_offset_mux);
    EL_LOGI(TAG, "Display center offset set to x=%d y=%d", x_offset, y_offset);
}

void eldra_eyes_get_display_center_offset(int *x_offset, int *y_offset)
{
    int dx = 0, dy = 0;
    snapshot_offsets(NULL, NULL, &dx, &dy);
    if (x_offset) *x_offset = dx;
    if (y_offset) *y_offset = dy;
}

void eldra_eyes_get_effective_center_offset(int *x_offset, int *y_offset)
{
    const int sprite_px_x = 11 * 14;
    const int sprite_px_y_nominal = 11 * 14;
    int cx = 0, cy = 0, dx = 0, dy = 0;
    snapshot_offsets(&cx, &cy, &dx, &dy);
    compute_effective_offsets(480, 480, sprite_px_x, sprite_px_y_nominal,
                              cx, cy, dx, dy, x_offset, y_offset);
}

void eldra_eyes_get_last_render_center_offset(int *x_offset, int *y_offset)
{
    if (x_offset) *x_offset = s_last_effective_eye_x_offset;
    if (y_offset) *y_offset = s_last_effective_eye_y_offset;
}

void eldra_eyes_get_activity(const eldra_eyes_context_t *ctx, eldra_eyes_activity_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!ctx) {
        return;
    }
    out->blink_active = ctx->blink.active;
    out->look_active = ctx->look.active;
    out->dizzy_active = ctx->reactive_dizzy.active;
    out->idle_clip_active = (ctx->idle.active_clip != IDLE_CLIP_NONE);
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

void eldra_eyes_set_modifiers(eldra_eyes_context_t *ctx, uint32_t modifier_mask)
{
    if (!ctx) {
        return;
    }
    ctx->modifiers = modifier_mask;
}

uint32_t eldra_eyes_get_modifiers(const eldra_eyes_context_t *ctx)
{
    if (!ctx) {
        return 0;
    }
    return ctx->modifiers;
}

void eldra_eyes_set_sleep_lid_heavy(eldra_eyes_context_t *ctx, bool heavy)
{
    if (!ctx) {
        return;
    }
    ctx->sleep_lid_heavy = heavy;
}

void eldra_eyes_set_lid_depths(uint8_t sleep_depth, uint8_t angry_depth)
{
    uint8_t clamped_sleep = (uint8_t)clamp_int((int)sleep_depth, 0, 6);
    uint8_t clamped_angry = (uint8_t)clamp_int((int)angry_depth, 0, 6);
    portENTER_CRITICAL(&s_offset_mux);
    s_sleep_lid_depth = clamped_sleep;
    s_angry_lid_depth = clamped_angry;
    portEXIT_CRITICAL(&s_offset_mux);
    EL_LOGI(TAG, "Lid depth set sleep=%u angry=%u", (unsigned)clamped_sleep, (unsigned)clamped_angry);
}

void eldra_eyes_get_lid_depths(uint8_t *sleep_depth, uint8_t *angry_depth)
{
    uint8_t sleep_v = 0;
    uint8_t angry_v = 0;
    portENTER_CRITICAL(&s_offset_mux);
    sleep_v = s_sleep_lid_depth;
    angry_v = s_angry_lid_depth;
    portEXIT_CRITICAL(&s_offset_mux);
    if (sleep_depth) *sleep_depth = sleep_v;
    if (angry_depth) *angry_depth = angry_v;
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
    float gdev = fabsf(accel_mag - 1.0f);

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

    /* Trigger dizzy from realistic shake patterns:
     *  - instant spike path (large gyro, or medium gyro + accel deviation),
     *  - accumulated motion path with decay instead of hard reset.
     */
    if (!ctx->reactive_dizzy.active && ctx->imu.cooldown_ms == 0) {
        bool instant_spike = (gyro_mag >= k_imu_gyro_dizzy_spike_dps) ||
                             (gyro_mag >= k_imu_gyro_dizzy_thresh_dps && gdev >= k_imu_gdev_dizzy_thresh);
        if (instant_spike) {
            eldra_eyes_trigger_dizzy(ctx, 0);
            ctx->imu.above_thresh_ms = 0;
            ctx->imu.cooldown_ms = k_dizzy_cooldown_ms;
            EL_LOGD(TAG, "IMU: Dizzy trigger spike (gyro=%.1f dps gdev=%.2f)",
                    (double)gyro_mag, (double)gdev);
        } else if (gyro_mag >= k_imu_gyro_dizzy_thresh_dps) {
            ctx->imu.above_thresh_ms += dt_ms;
            if (ctx->imu.above_thresh_ms >= k_imu_gyro_dizzy_time_ms) {
                eldra_eyes_trigger_dizzy(ctx, 0);
                ctx->imu.above_thresh_ms = 0;
                ctx->imu.cooldown_ms = k_dizzy_cooldown_ms;
                EL_LOGD(TAG, "IMU: Dizzy trigger accumulated (gyro=%.1f dps gdev=%.2f)",
                        (double)gyro_mag, (double)gdev);
            }
        } else if (gyro_mag < k_imu_gyro_decay_floor_dps) {
            // Decay toward zero so brief dips don't cancel a near-threshold shake.
            uint32_t decay = dt_ms * 2U;
            if (ctx->imu.above_thresh_ms > decay) {
                ctx->imu.above_thresh_ms -= decay;
            } else {
                ctx->imu.above_thresh_ms = 0;
            }
        } else {
            // In the shoulder band, hold accumulator steady.
        }
    }

    /* Whip detection removed */

    /* Periodic debug log */
    if (ctx->imu.log_timer_ms > dt_ms) {
        ctx->imu.log_timer_ms -= dt_ms;
    } else {
        float roll_deg = atan2f(ay_g, az_g) * k_rad_to_deg;
        float pitch_deg = atan2f(-ax_g, sqrtf(ay_g * ay_g + az_g * az_g)) * k_rad_to_deg;
        EL_LOGD(TAG, "IMU: gyro=%.1f dps (ema=%.1f) accel=%.2f g gdev=%.2f (ema=%.2f) roll=%.1f pitch=%.1f acc=%ums/%0.1fdps cool=%ums",
                 (double)gyro_mag,
                 (double)ctx->imu.gyro_ema_dps,
                 (double)accel_mag,
                 (double)gdev,
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
        if (ctx->idle.since_blink_ms <= (UINT32_MAX - dt_ms)) {
            ctx->idle.since_blink_ms += dt_ms;
        } else {
            ctx->idle.since_blink_ms = UINT32_MAX;
        }
        if (ctx->idle.since_look_ms <= (UINT32_MAX - dt_ms)) {
            ctx->idle.since_look_ms += dt_ms;
        } else {
            ctx->idle.since_look_ms = UINT32_MAX;
        }

        // Failsafe cadence: never let random idle selection suppress blink/look
        // for long stretches.
        if (ctx->idle.since_look_ms >= k_idle_force_look_ms) {
            ctx->idle.idle_gap_ms = 0;
            ctx->idle.last_clip = IDLE_CLIP_NONE;
            ctx->idle.active_clip = IDLE_CLIP_NONE;
            ctx->look.active = false;
            idle_pick_next(ctx);
            if (ctx->idle.active_clip != IDLE_CLIP_LOOK) {
                // Force a look clip if random picker chose otherwise.
                ctx->idle.active_clip = IDLE_CLIP_LOOK;
                ctx->idle.clip_elapsed_ms = 0;
                ctx->blink.active = false;
                ctx->blink.elapsed_ms = 0;
                ctx->look.active = true;
                ctx->look.elapsed_ms = 0;
                ctx->look.direction = (esp_random() & 0x01) ? 1 : -1;
                ctx->look.frame_elapsed_ms = 0;
                ctx->look.frame_index = 0;
                ctx->look.duration_ms = look_sequence_total_ms(k_look_right_sequence, k_look_right_sequence_len);
                ctx->idle.clip_duration_ms = ctx->look.duration_ms;
                ctx->idle.since_look_ms = 0;
                EL_LOGD(TAG, "Idle forced: LOOK dir=%s", (ctx->look.direction >= 0) ? "RIGHT" : "LEFT");
            }
        } else if (ctx->idle.since_blink_ms >= k_idle_force_blink_ms) {
            ctx->idle.active_clip = IDLE_CLIP_BLINK;
            ctx->idle.clip_elapsed_ms = 0;
            ctx->look.active = false;
            ctx->look.elapsed_ms = 0;
            ctx->look.frame_elapsed_ms = 0;
            ctx->look.frame_index = 0;
            ctx->blink.active = true;
            ctx->blink.elapsed_ms = 0;
            ctx->blink.duration_ms = 140;
            ctx->blink.gap_ms = 0;
            ctx->blink.repeat_count = 1;
            ctx->blink.hold_ms = 0;
            ctx->idle.clip_duration_ms = blink_total_ms(ctx->blink.duration_ms,
                                                        ctx->blink.hold_ms,
                                                        ctx->blink.gap_ms,
                                                        ctx->blink.repeat_count);
            ctx->idle.since_blink_ms = 0;
            EL_LOGD(TAG, "Idle forced: BLINK");
        } else if (ctx->idle.idle_gap_ms > dt_ms) {
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

    if (s_test_pattern) {
        int req_x = 0, req_y = 0, disp_x = 0, disp_y = 0;
        snapshot_offsets(&req_x, &req_y, &disp_x, &disp_y);
        const int center_y = (fb_height / 2) + disp_y + req_y;
        const int center_x = (fb_width / 2) + disp_x + req_x;
        draw_test_pattern(framebuffer, fb_width, fb_height, center_x, center_y);
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
    const int eye_offset = (sprite_px_x * 3) / 4; // Horizontal offset from center

    int req_x = 0, req_y = 0, disp_x = 0, disp_y = 0;
    snapshot_offsets(&req_x, &req_y, &disp_x, &disp_y);

    int clamped_x = 0;
    int clamped_y = 0;
    compute_effective_offsets(fb_width, fb_height, sprite_px_x, sprite_px_y_nominal,
                              req_x, req_y, disp_x, disp_y,
                              &clamped_x, &clamped_y);
    s_last_effective_eye_x_offset = clamped_x;
    s_last_effective_eye_y_offset = clamped_y;

    const int center_y = (fb_height / 2) + disp_y + clamped_y;
    const int center_x = (fb_width / 2) + disp_x + clamped_x;

    const int base_y0 = center_y - (sprite_px_y_nominal / 2);
    int left_y0 = base_y0 + breath_offset_left_px;
    int right_y0 = base_y0 + breath_offset_right_px;

    const uint8_t (*blink_frame)[11] = eye_frame;
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
                float closure = 0.0f;
                if (t_in_cycle < close_ms) {
                    closure = (float)t_in_cycle / (float)close_ms;
                } else if (t_in_cycle < close_ms + ctx->blink.hold_ms) {
                    closure = 1.0f;
                } else if (t_in_cycle < close_ms + ctx->blink.hold_ms + open_ms) {
                    uint32_t t_open = t_in_cycle - (close_ms + ctx->blink.hold_ms);
                    closure = 1.0f - ((float)t_open / (float)open_ms);
                } else {
                    closure = 0.0f;
                }

                if (closure < 0.35f) {
                    blink_frame = eye_frame;
                } else if (closure < 0.70f) {
                    blink_frame = k_eye_frame_blink_mid;
                } else if (closure < 0.95f) {
                    blink_frame = k_eye_frame_blink_slim;
                } else {
                    blink_frame = k_eye_frame_closed;
                }
            }
        }
    }

    uint8_t tired_frame[11][11];
    uint8_t modifier_frame[11][11];
    uint8_t sleep_lid_depth = 2;
    uint8_t angry_lid_depth = 2;
    eldra_eyes_get_lid_depths(&sleep_lid_depth, &angry_lid_depth);
    const uint32_t mods = ctx->modifiers;
    const bool mod_sleepy = (mods & ELDRA_EYES_MOD_SLEEPY) != 0U;
    const bool mod_hungry = (mods & ELDRA_EYES_MOD_HUNGRY) != 0U;
    const bool mod_angry_base = (ctx->mood == ELDRA_EYES_MOOD_ANGRY);

    // Layering model:
    // 1) sleep countdown heavy (final 60s) wins and stays rounded/hooded.
    // 2) otherwise apply sleepy need as a lighter lid.
    // 3) angry base mood remains available, including angry+sleepy blend.
    if (!reactive_dizzy && !ctx->blink.active) {
        if (ctx->sleep_lid_heavy) {
            int lid_depth = (int)sleep_lid_depth;
            if (breath_enabled) {
                if (breath_wave > 0.30f) {
                    lid_depth += 1;
                } else if (breath_wave < -0.30f) {
                    lid_depth -= 1;
                }
            }
            build_tired_frame(eye_frame, tired_frame, (uint8_t)clamp_int(lid_depth, 0, 6), false);
            blink_frame = tired_frame;
            left_y0 += 1;
            right_y0 += 1;
        } else if (mod_angry_base && mod_sleepy) {
            uint8_t blend_depth = (uint8_t)clamp_int((int)angry_lid_depth - 1, 0, 6);
            build_tired_frame(eye_frame, tired_frame, blend_depth, true);
            blink_frame = tired_frame;
            left_y0 += 1;
            right_y0 += 1;
            scale_y_left = clamp_int(scale_y_left - 1, 1, 255);
            scale_y_right = clamp_int(scale_y_right - 1, 1, 255);
        } else if (mod_sleepy) {
            uint8_t light_depth = (uint8_t)clamp_int((int)sleep_lid_depth - 1, 0, 6);
            build_tired_frame(eye_frame, tired_frame, light_depth, false);
            blink_frame = tired_frame;
            left_y0 += 1;
            right_y0 += 1;
        } else if (mod_angry_base) {
            build_tired_frame(eye_frame, tired_frame, angry_lid_depth, true);
            blink_frame = tired_frame;
            left_y0 += 1;
            right_y0 += 1;
            scale_y_left = clamp_int(scale_y_left - 1, 1, 255);
            scale_y_right = clamp_int(scale_y_right - 1, 1, 255);
        }
    }

    const uint8_t (*final_frame)[11] = blink_frame;
    if (mod_hungry && !reactive_dizzy) {
        apply_hungry_tint(blink_frame, modifier_frame);
        final_frame = modifier_frame;
    }

    if (scale_y_left < 1) scale_y_left = 1;
    if (scale_y_right < 1) scale_y_right = 1;

    int left_x0 = (center_x - eye_offset) - (sprite_px_x / 2);
    int right_x0 = (center_x + eye_offset) - (sprite_px_x / 2);

    if (reactive_dizzy) {
        float spin_phase = ((float)ctx->reactive_dizzy.elapsed_ms / 1000.0f) * (k_two_pi * 2.5f); // ~2.5 turns per second
        blit_eye_spiral(framebuffer, fb_width, fb_height,
                        left_x0, left_y0, scale_x, scale_y_left, spin_phase);
        blit_eye_spiral(framebuffer, fb_width, fb_height,
                        right_x0, right_y0, scale_x, scale_y_right, spin_phase + 0.4f);
    } else {
        blit_eye(framebuffer, fb_width, fb_height,
                 left_x0, left_y0, scale_x, scale_y_left, final_frame);
        blit_eye(framebuffer, fb_width, fb_height,
                 right_x0, right_y0, scale_x, scale_y_right, final_frame);
    }
}

static void copy_eye_frame(uint8_t dst[11][11], const uint8_t src[11][11])
{
    for (int row = 0; row < 11; ++row) {
        for (int col = 0; col < 11; ++col) {
            dst[row][col] = src[row][col];
        }
    }
}

static void apply_hungry_tint(const uint8_t src[11][11], uint8_t dst[11][11])
{
    for (int row = 0; row < 11; ++row) {
        for (int col = 0; col < 11; ++col) {
            uint8_t px = src[row][col];
            // Dim highlights a notch when hungry to convey lower vitality.
            if (px == 7) {
                px = 6;
            } else if (px == 6) {
                px = 5;
            }
            dst[row][col] = px;
        }
    }
}

static void build_tired_frame(const uint8_t src[11][11], uint8_t dst[11][11], uint8_t lid_depth, bool heavy)
{
    // Curved upper-lid overlay using fixed per-column cut profiles.
    // This avoids procedural edge artifacts ("tails"/pillars) when look
    // frames shift left/right.
    copy_eye_frame(dst, src);

    static const uint8_t k_cut_light[11] = {0, 0, 1, 1, 1, 2, 1, 1, 1, 0, 0};
    static const uint8_t k_cut_heavy[11] = {1, 1, 2, 2, 3, 4, 3, 2, 2, 1, 1};

    for (int col = 0; col < 11; ++col) {
        int lid_row = (heavy ? k_cut_heavy[col] : k_cut_light[col]) + (int)lid_depth;
        if (lid_row < 1) lid_row = 1;
        if (lid_row > 9) lid_row = 9;

        for (int row = 0; row < 11; ++row) {
            if (src[row][col] == 0) {
                continue;
            }
            if (row < lid_row) {
                dst[row][col] = 0;
            } else if (row == lid_row) {
                // Keep hard lid edge near the center to preserve rounded corners.
                if (col >= 3 && col <= 7) {
                    dst[row][col] = 3; // soft lid edge
                } else if (dst[row][col] != 7 && dst[row][col] < 3) {
                    dst[row][col] = 3;
                }
            } else if (row == lid_row + 1 && col >= 4 && col <= 6) {
                // Soft blend line; preserve bright specular pixels.
                if (dst[row][col] != 7 && dst[row][col] < 3) {
                    dst[row][col] = 3;
                }
            }
        }
    }
}


