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

static eldra_asset_provider_t s_provider = {0};
static glyph_data_t s_glyph_sleep = {0};
static bool s_visible = false;
static eldra_glyph_id_t s_current = GLYPH_SLEEP;
static int s_offset_x = 0;
static int s_offset_y = -20;

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

    int w = 0, h = 0;
    const uint8_t *rgba = s_provider.get_glyph_rgba(GLYPH_SLEEP, &w, &h);
    if (rgba && w > 0 && h > 0) {
        s_glyph_sleep.w = w;
        s_glyph_sleep.h = h;
        s_glyph_sleep.rgba = rgba;
    }
}

void eldra_glyphs_show(eldra_glyph_id_t id)
{
    s_current = id;
    s_visible = true;
}

void eldra_glyphs_hide(void)
{
    s_visible = false;
}

void eldra_glyphs_set_offset(int dx, int dy)
{
    s_offset_x = dx;
    s_offset_y = dy;
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
                      float intensity)
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
            *dst = rgb565_blend(*dst, GLYPH_COLOR_R, GLYPH_COLOR_G, GLYPH_COLOR_B, a);
        }
    }
}

void eldra_glyphs_render(uint16_t *fb, int fb_w, int fb_h, uint64_t now_ms)
{
    if (!s_visible || !fb) return;
    const glyph_data_t *g = &s_glyph_sleep;
    if (!g->rgba) return;

    float phase = (float)(now_ms % 4000ULL) / 4000.0f * 6.28318f;
    float intensity = 0.15f + sinf(phase) * 0.10f;
    if (intensity < 0.02f) intensity = 0.02f;

    int cx = fb_w / 2 + s_offset_x - g->w / 2;
    int cy = fb_h / 3 + s_offset_y - g->h / 2;

    // soft glow: three passes
    draw_mask(fb, fb_w, fb_h, g, cx - 1, cy, intensity * 0.25f);
    draw_mask(fb, fb_w, fb_h, g, cx + 1, cy, intensity * 0.25f);
    draw_mask(fb, fb_w, fb_h, g, cx, cy + 1, intensity * 0.25f);
    draw_mask(fb, fb_w, fb_h, g, cx, cy - 1, intensity * 0.25f);
    // core
    draw_mask(fb, fb_w, fb_h, g, cx, cy, intensity);
}
