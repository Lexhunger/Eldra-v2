#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GLYPH_SLEEP = 0,
} eldra_glyph_id_t;

typedef struct {
    const uint8_t *(*get_glyph_rgba)(eldra_glyph_id_t id, int *width, int *height);
} eldra_asset_provider_t;

void eldra_glyphs_init(const eldra_asset_provider_t *provider);
void eldra_glyphs_show(eldra_glyph_id_t id);
void eldra_glyphs_hide(void);
void eldra_glyphs_set_offset(int dx, int dy);
void eldra_glyphs_render(uint16_t *fb, int fb_w, int fb_h, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
