#include "eldra_glyphs.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "glyph_sleep_rgba.h"
#include "esp_log.h"
#include "eldra_logging.h"

#define GLYPH_COLOR_R 40
#define GLYPH_COLOR_G 90
#define GLYPH_COLOR_B 140

typedef struct {
    int w;
    int h;
    const uint8_t *rgba; // 4-byte RGBA, premultiplied not required
} glyph_data_t;

typedef struct {
    eldra_glyph_id_t id;
    const char *name;
    const uint8_t *rgba;
    int w;
    int h;
    int vis_min_x;
    int vis_min_y;
    int vis_max_x;
    int vis_max_y;
    int default_dx;
    int default_dy;
    uint8_t tint_r, tint_g, tint_b;
} glyph_desc_t;

#define MAX_SLOTS 4
typedef struct {
    bool visible;
    const glyph_desc_t *desc;
    int offset_x;
    int offset_y;
    bool force; // if true, never auto-hidden
} glyph_slot_t;

static eldra_asset_provider_t s_provider = {0};
static glyph_desc_t s_desc_sleep = {
    .id = GLYPH_SLEEP,
    .name = "sleep",
    .rgba = glyph_sleep_rgba,
    .w = (int)glyph_sleep_w,
    .h = (int)glyph_sleep_h,
    .vis_min_x = 0,
    .vis_min_y = 0,
    .vis_max_x = (int)glyph_sleep_w - 1,
    .vis_max_y = (int)glyph_sleep_h - 1,
    .default_dx = 0,
    .default_dy = -170, // top outer-edge default for 480x480 center-based layout
    .tint_r = GLYPH_COLOR_R,
    .tint_g = GLYPH_COLOR_G,
    .tint_b = GLYPH_COLOR_B,
};

static const glyph_desc_t *s_descs[] = {
    &s_desc_sleep,
};

static glyph_slot_t s_slots[MAX_SLOTS];
static eldra_glyph_layout_t s_layout = GLYPH_LAYOUT_STACK;
static int s_scale = 2;
static int s_disp_dx = 0;
static int s_disp_dy = 0;
static int s_eye_dx = 0;
static int s_eye_dy = 0;
static bool s_initialized = false;
static const char *TAG = "eldra_glyphs";
static uint32_t s_last_dbg_ms = 0;
static const uint32_t k_pulse_period_ms = 12000; // calmer pulse cadence

static const uint8_t *embedded_get(eldra_glyph_id_t id, int *w, int *h)
{
    if (id != GLYPH_SLEEP) return NULL;
    if (w) *w = (int)glyph_sleep_w;
    if (h) *h = (int)glyph_sleep_h;
    return glyph_sleep_rgba;
}

static void compute_visible_bounds(glyph_desc_t *d)
{
    if (!d || !d->rgba || d->w <= 0 || d->h <= 0) {
        return;
    }

    int min_x = d->w;
    int min_y = d->h;
    int max_x = -1;
    int max_y = -1;

    for (int y = 0; y < d->h; ++y) {
        for (int x = 0; x < d->w; ++x) {
            const uint8_t *px = d->rgba + ((y * d->w + x) * 4);
            if (px[3] == 0) continue;
            if (x < min_x) min_x = x;
            if (y < min_y) min_y = y;
            if (x > max_x) max_x = x;
            if (y > max_y) max_y = y;
        }
    }

    if (max_x < 0 || max_y < 0) {
        d->vis_min_x = 0;
        d->vis_min_y = 0;
        d->vis_max_x = d->w - 1;
        d->vis_max_y = d->h - 1;
        return;
    }

    d->vis_min_x = min_x;
    d->vis_min_y = min_y;
    d->vis_max_x = max_x;
    d->vis_max_y = max_y;
}

void eldra_glyphs_init(const eldra_asset_provider_t *provider)
{
    if (s_initialized && !provider) {
        return;
    }

    if (provider) {
        s_provider = *provider;
    } else {
        s_provider.get_glyph_rgba = embedded_get;
    }

    // Load all known glyphs (currently only sleep).
    for (size_t i = 0; i < sizeof(s_descs)/sizeof(s_descs[0]); ++i) {
        glyph_desc_t *d = (glyph_desc_t *)s_descs[i];
        int w = 0, h = 0;
        const uint8_t *rgba = s_provider.get_glyph_rgba(d->id, &w, &h);
        if (rgba && w > 0 && h > 0) {
            d->rgba = rgba;
            d->w = w;
            d->h = h;
        }
        compute_visible_bounds(d);
    }

    // Reset runtime state to deterministic defaults on init.
    s_layout = GLYPH_LAYOUT_STACK;
    s_scale = 2;
    s_disp_dx = 0;
    s_disp_dy = 0;
    s_eye_dx = 0;
    s_eye_dy = 0;
    s_last_dbg_ms = 0;

    // Reset slots to defaults (slot0 shows nothing by default).
    for (int i = 0; i < MAX_SLOTS; ++i) {
        s_slots[i].visible = false;
        s_slots[i].desc = NULL;
        s_slots[i].offset_x = 0;
        s_slots[i].offset_y = 0;
        s_slots[i].force = false;
    }
    s_initialized = true;
}

bool eldra_glyphs_is_initialized(void)
{
    return s_initialized;
}

static const glyph_desc_t *lookup_desc_by_id(eldra_glyph_id_t id)
{
    for (size_t i = 0; i < sizeof(s_descs)/sizeof(s_descs[0]); ++i) {
        if (s_descs[i]->id == id) return s_descs[i];
    }
    return NULL;
}

void eldra_glyphs_set_layout(eldra_glyph_layout_t layout)
{
    s_layout = layout;
}

void eldra_glyphs_show(eldra_glyph_id_t id, int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) slot = 0;
    const glyph_desc_t *d = lookup_desc_by_id(id);
    if (!d || !d->rgba) return;
    bool was_forced = s_slots[slot].force;
    bool keep_offset = (s_slots[slot].desc != NULL);
    s_slots[slot].desc = d;
    s_slots[slot].visible = true;
    // Preserve caller-configured offsets (e.g., persisted config); only apply defaults on first use.
    if (!keep_offset) {
        s_slots[slot].offset_x = d->default_dx;
        s_slots[slot].offset_y = d->default_dy;
    }
    // Preserve manual force pinning if this slot was explicitly forced earlier.
    s_slots[slot].force = was_forced;
}

void eldra_glyphs_force_show(eldra_glyph_id_t id, int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) slot = 0;
    const glyph_desc_t *d = lookup_desc_by_id(id);
    if (!d || !d->rgba) return;
    bool keep_offset = (s_slots[slot].desc != NULL);
    s_slots[slot].desc = d;
    s_slots[slot].visible = true;
    // Preserve caller-configured offsets (e.g., persisted config); only apply defaults on first use.
    if (!keep_offset) {
        s_slots[slot].offset_x = d->default_dx;
        s_slots[slot].offset_y = d->default_dy;
    }
    s_slots[slot].force = true;
}

void eldra_glyphs_hide(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) {
        for (int i = 0; i < MAX_SLOTS; ++i) { s_slots[i].visible = false; s_slots[i].force = false; }
        return;
    }
    s_slots[slot].visible = false;
    s_slots[slot].force = false;
}

void eldra_glyphs_hide_all(void)
{
    for (int i = 0; i < MAX_SLOTS; ++i) { s_slots[i].visible = false; s_slots[i].force = false; }
}

void eldra_glyphs_hide_unforced(void)
{
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (!s_slots[i].force) s_slots[i].visible = false;
    }
}

void eldra_glyphs_set_offset(int slot, int dx, int dy)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    s_slots[slot].offset_x = dx;
    s_slots[slot].offset_y = dy;
}

void eldra_glyphs_set_scale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6; // avoid huge overdraw
    s_scale = scale;
}

void eldra_glyphs_set_display_center_offset(int dx, int dy)
{
    s_disp_dx = dx;
    s_disp_dy = dy;
}

void eldra_glyphs_set_eye_center_offset(int dx, int dy)
{
    s_eye_dx = dx;
    s_eye_dy = dy;
}

static uint16_t rgb565_blend(uint16_t dst, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    uint8_t dr = (dst >> 11) & 0x1F;
    uint8_t dg = (dst >> 5) & 0x3F;
    uint8_t db = dst & 0x1F;
    dr = (dr * 255) / 31;
    dg = (dg * 255) / 63;
    db = (db * 255) / 31;
    uint8_t nr = (uint8_t)((r * alpha + dr * (255 - alpha)) / 255);
    uint8_t ng = (uint8_t)((g * alpha + dg * (255 - alpha)) / 255);
    uint8_t nb = (uint8_t)((b * alpha + db * (255 - alpha)) / 255);
    return (uint16_t)(((nr & 0xF8) << 8) | ((ng & 0xFC) << 3) | (nb >> 3));
}

static void draw_mask(uint16_t *fb, int fb_w, int fb_h,
                      const glyph_data_t *glyph, int origin_x, int origin_y,
                      float intensity, uint8_t tint_r, uint8_t tint_g, uint8_t tint_b, int scale)
{
    if (!fb || !glyph || !glyph->rgba || scale <= 0) return;
    int scaled_h = glyph->h * scale;
    int scaled_w = glyph->w * scale;
    for (int y = 0; y < scaled_h; ++y) {
        int dy = origin_y + y;
        if (dy < 0 || dy >= fb_h) continue;
        const int src_y = y / scale;
        for (int x = 0; x < scaled_w; ++x) {
            int dx = origin_x + x;
            if (dx < 0 || dx >= fb_w) continue;
            const int src_x = x / scale;
            const uint8_t *px = glyph->rgba + (src_y * glyph->w + src_x) * 4;
            uint8_t a = (uint8_t)(px[3] * intensity);
            if (a == 0) continue;
            uint16_t *dst = fb + dy * fb_w + dx;
            *dst = rgb565_blend(*dst, tint_r, tint_g, tint_b, a);
        }
    }
}

void eldra_glyphs_render(uint16_t *fb, int fb_w, int fb_h, uint64_t now_ms)
{
    if (!fb) return;

    float phase = (float)(now_ms % k_pulse_period_ms) / (float)k_pulse_period_ms * 6.28318f;
    float intensity = 0.45f + sinf(phase) * 0.20f; // stronger core so glyph stays visible over bright eyes
    if (intensity < 0.15f) intensity = 0.15f;
    if (intensity > 0.85f) intensity = 0.85f;

    // Count active slots
    int active = 0;
    for (int i = 0; i < MAX_SLOTS; ++i) if (s_slots[i].visible && s_slots[i].desc) active++;
    if (active == 0) return;

    int idx = 0;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        glyph_slot_t *slot = &s_slots[i];
        if (!slot->visible || !slot->desc || !slot->desc->rgba) continue;

        glyph_data_t g = { slot->desc->w, slot->desc->h, slot->desc->rgba };
        int vis_w = (slot->desc->vis_max_x - slot->desc->vis_min_x + 1) * s_scale;
        int vis_h = (slot->desc->vis_max_y - slot->desc->vis_min_y + 1) * s_scale;

        int center_x = fb_w / 2 + s_disp_dx + s_eye_dx + slot->offset_x;
        int center_y = fb_h / 2 + s_disp_dy + s_eye_dy + slot->offset_y;

        if (s_layout == GLYPH_LAYOUT_RING && active > 1) {
            float ang = ((float)idx / active) * 6.28318f - 1.5708f; // start at top
            int radius = (fb_w < fb_h ? fb_w : fb_h) / 2 - vis_w;
            center_x = fb_w / 2 + s_disp_dx + s_eye_dx + (int)(cosf(ang) * radius) + slot->offset_x;
            center_y = fb_h / 2 + s_disp_dy + s_eye_dy + (int)(sinf(ang) * radius) + slot->offset_y;
        } else if (s_layout == GLYPH_LAYOUT_STACK && active > 1) {
            int spacing = vis_h + 4;
            center_y = fb_h / 2 + s_disp_dy + s_eye_dy - (active - 1) * spacing / 2 + idx * spacing + slot->offset_y;
        }

        // Center based on visible alpha bounds (not raw image bounds),
        // then convert back to full-image origin for draw_mask().
        int vis_tl_x = center_x - vis_w / 2;
        int vis_tl_y = center_y - vis_h / 2;
        int origin_x = vis_tl_x - (slot->desc->vis_min_x * s_scale);
        int origin_y = vis_tl_y - (slot->desc->vis_min_y * s_scale);

        // soft glow: four low-alpha passes + core
        draw_mask(fb, fb_w, fb_h, &g, origin_x - 1, origin_y, intensity * 0.25f,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b, s_scale);
        draw_mask(fb, fb_w, fb_h, &g, origin_x + 1, origin_y, intensity * 0.25f,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b, s_scale);
        draw_mask(fb, fb_w, fb_h, &g, origin_x, origin_y + 1, intensity * 0.25f,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b, s_scale);
        draw_mask(fb, fb_w, fb_h, &g, origin_x, origin_y - 1, intensity * 0.25f,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b, s_scale);
        draw_mask(fb, fb_w, fb_h, &g, origin_x, origin_y, intensity,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b, s_scale);
        idx++;

        uint32_t now32 = (uint32_t)(now_ms & 0xFFFFFFFFU);
        if (now32 - s_last_dbg_ms > 5000U) {
            EL_LOGD(TAG, "glyph id=%d center=(%d,%d) origin=(%d,%d) scale=%d active=%d",
                    slot->desc->id, center_x, center_y, origin_x, origin_y, s_scale, active);
            s_last_dbg_ms = now32;
        }
    }
}
