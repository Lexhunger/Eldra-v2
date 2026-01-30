#include "eldra_glyphs.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "glyph_sleep_rgba.h"

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
} glyph_slot_t;

static eldra_asset_provider_t s_provider = {0};
static glyph_data_t s_glyph_sleep = {0};
static glyph_desc_t s_desc_sleep = {
    .id = GLYPH_SLEEP,
    .name = "sleep",
    .rgba = glyph_sleep_rgba,
    .w = (int)glyph_sleep_w,
    .h = (int)glyph_sleep_h,
    .default_dx = 0,
    .default_dy = -20,
    .tint_r = GLYPH_COLOR_R,
    .tint_g = GLYPH_COLOR_G,
    .tint_b = GLYPH_COLOR_B,
};

static const glyph_desc_t *s_descs[] = {
    &s_desc_sleep,
};

static glyph_slot_t s_slots[MAX_SLOTS];
static eldra_glyph_layout_t s_layout = GLYPH_LAYOUT_STACK;

static const uint8_t *embedded_get(eldra_glyph_id_t id, int *w, int *h)
{
    if (id != GLYPH_SLEEP) return NULL;
    if (w) *w = (int)glyph_sleep_w;
    if (h) *h = (int)glyph_sleep_h;
    return glyph_sleep_rgba;
}

void eldra_glyphs_init(const eldra_asset_provider_t *provider)
{
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
    }

    // Reset slots to defaults (slot0 shows nothing by default).
    for (int i = 0; i < MAX_SLOTS; ++i) {
        s_slots[i].visible = false;
        s_slots[i].desc = NULL;
        s_slots[i].offset_x = 0;
        s_slots[i].offset_y = 0;
    }
}

static const glyph_desc_t *lookup_desc_by_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(s_descs)/sizeof(s_descs[0]); ++i) {
        if (strcmp(s_descs[i]->name, name) == 0) return s_descs[i];
    }
    return NULL;
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
    s_slots[slot].desc = d;
    s_slots[slot].visible = true;
    s_slots[slot].offset_x = d->default_dx;
    s_slots[slot].offset_y = d->default_dy;
}

void eldra_glyphs_hide(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) {
        for (int i = 0; i < MAX_SLOTS; ++i) s_slots[i].visible = false;
        return;
    }
    s_slots[slot].visible = false;
}

void eldra_glyphs_hide_all(void)
{
    for (int i = 0; i < MAX_SLOTS; ++i) s_slots[i].visible = false;
}

void eldra_glyphs_set_offset(int slot, int dx, int dy)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    s_slots[slot].offset_x = dx;
    s_slots[slot].offset_y = dy;
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
                      float intensity, uint8_t tint_r, uint8_t tint_g, uint8_t tint_b)
{
    if (!fb || !glyph || !glyph->rgba) return;
    for (int y = 0; y < glyph->h; ++y) {
        int dy = origin_y + y;
        if (dy < 0 || dy >= fb_h) continue;
        for (int x = 0; x < glyph->w; ++x) {
            int dx = origin_x + x;
            if (dx < 0 || dx >= fb_w) continue;
            const uint8_t *px = glyph->rgba + (y * glyph->w + x) * 4;
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

    float phase = (float)(now_ms % 4000ULL) / 4000.0f * 6.28318f;
    float intensity = 0.15f + sinf(phase) * 0.10f;
    if (intensity < 0.02f) intensity = 0.02f;

    // Count active slots
    int active = 0;
    for (int i = 0; i < MAX_SLOTS; ++i) if (s_slots[i].visible && s_slots[i].desc) active++;
    if (active == 0) return;

    int idx = 0;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        glyph_slot_t *slot = &s_slots[i];
        if (!slot->visible || !slot->desc || !slot->desc->rgba) continue;

        glyph_data_t g = { slot->desc->w, slot->desc->h, slot->desc->rgba };
        int cx = fb_w/2 + slot->offset_x - g.w/2;
        int cy = fb_h/3 + slot->offset_y - g.h/2;

        if (s_layout == GLYPH_LAYOUT_RING && active > 1) {
            float ang = ((float)idx / active) * 6.28318f - 1.5708f; // start at top
            int radius = (fb_w < fb_h ? fb_w : fb_h) / 2 - g.w;
            cx = fb_w/2 + (int)(cosf(ang) * radius) - g.w/2 + slot->offset_x;
            cy = fb_h/2 + (int)(sinf(ang) * radius) - g.h/2 + slot->offset_y;
        } else if (s_layout == GLYPH_LAYOUT_STACK && active > 1) {
            int spacing = g.h + 4;
            cy = fb_h/4 - (active-1)*spacing/2 + idx*spacing + slot->offset_y;
        }

        // soft glow: three passes
        draw_mask(fb, fb_w, fb_h, &g, cx - 1, cy, intensity * 0.25f,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b);
        draw_mask(fb, fb_w, fb_h, &g, cx + 1, cy, intensity * 0.25f,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b);
        draw_mask(fb, fb_w, fb_h, &g, cx, cy + 1, intensity * 0.25f,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b);
        draw_mask(fb, fb_w, fb_h, &g, cx, cy - 1, intensity * 0.25f,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b);
        // core
        draw_mask(fb, fb_w, fb_h, &g, cx, cy, intensity,
                  slot->desc->tint_r, slot->desc->tint_g, slot->desc->tint_b);
        idx++;
    }
}
