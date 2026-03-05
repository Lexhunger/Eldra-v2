#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GLYPH_SLEEP = 0,
    GLYPH_MAX
} eldra_glyph_id_t;

typedef struct {
    const uint8_t *(*get_glyph_rgba)(eldra_glyph_id_t id, int *width, int *height);
} eldra_asset_provider_t;

typedef enum {
    GLYPH_LAYOUT_STACK = 0, /* vertical stack along the centerline */
    GLYPH_LAYOUT_RING  = 1, /* spread like a clock around the rim */
} eldra_glyph_layout_t;

void eldra_glyphs_set_layout(eldra_glyph_layout_t layout);
void eldra_glyphs_init(const eldra_asset_provider_t *provider);
bool eldra_glyphs_is_initialized(void);
void eldra_glyphs_show(eldra_glyph_id_t id, int slot);
void eldra_glyphs_force_show(eldra_glyph_id_t id, int slot);
void eldra_glyphs_hide(int slot);
void eldra_glyphs_hide_all(void);
void eldra_glyphs_hide_unforced(void);
void eldra_glyphs_set_offset(int slot, int dx, int dy);
void eldra_glyphs_set_scale(int scale); /* integer scale >=1 */
void eldra_glyphs_set_display_center_offset(int dx, int dy);
void eldra_glyphs_set_eye_center_offset(int dx, int dy);
void eldra_glyphs_render(uint16_t *fb, int fb_w, int fb_h, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
