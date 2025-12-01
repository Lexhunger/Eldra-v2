/**
 * @file eldra_eyes.c
 * @brief Eye rendering implementation with 11x11 pixel art eye
 * 
 * Features an 8-color RGB565 palette and scaling support.
 */

#include "eldra_eyes.h"
#include "eldra_display_round.h"
#include "esp_log.h"

static const char *TAG = "eldra_eyes";

/* Eye dimensions */
#define EYE_SIZE 11

/**
 * 8-color RGB565 palette
 * Colors are stored in RGB565 format: RRRRRGGGGGGBBBBB
 */
#define PALETTE_SIZE 8
static const uint16_t s_palette[PALETTE_SIZE] = {
    0x0000,  /* BLACK   - 0 */
    0xFFFF,  /* WHITE   - 1 */
    0x001F,  /* BLUE    - 2 */
    0xF800,  /* RED     - 3 */
    0x07E0,  /* GREEN   - 4 */
    0xFFE0,  /* YELLOW  - 5 */
    0x07FF,  /* CYAN    - 6 */
    0xF81F,  /* MAGENTA - 7 */
};

/**
 * 11x11 eye bitmap using palette indices
 * Legend: 0=Black, 1=White, 2=Blue
 * 
 * The eye design:
 * - Outer ring: Black outline
 * - Eye white: White fill
 * - Iris: Blue
 * - Pupil: Black center
 */
static const uint8_t s_eye_bitmap[EYE_SIZE][EYE_SIZE] = {
    /* Row 0  */ {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0},
    /* Row 1  */ {0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    /* Row 2  */ {0, 1, 1, 1, 2, 2, 2, 1, 1, 1, 0},
    /* Row 3  */ {0, 1, 1, 2, 2, 2, 2, 2, 1, 1, 0},
    /* Row 4  */ {1, 1, 2, 2, 2, 0, 2, 2, 2, 1, 1},
    /* Row 5  */ {1, 1, 2, 2, 0, 0, 0, 2, 2, 1, 1},
    /* Row 6  */ {1, 1, 2, 2, 2, 0, 2, 2, 2, 1, 1},
    /* Row 7  */ {0, 1, 1, 2, 2, 2, 2, 2, 1, 1, 0},
    /* Row 8  */ {0, 1, 1, 1, 2, 2, 2, 1, 1, 1, 0},
    /* Row 9  */ {0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    /* Row 10 */ {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0},
};

static bool s_initialized = false;

esp_err_t eldra_eyes_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Eyes module already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing eyes module");
    s_initialized = true;
    
    return ESP_OK;
}

esp_err_t eldra_eyes_draw(const eldra_eye_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->scale < 1) {
        ESP_LOGE(TAG, "Invalid scale: %d", config->scale);
        return ESP_ERR_INVALID_ARG;
    }

    int scaled_size = EYE_SIZE * config->scale;
    int start_x = config->center_x - (scaled_size / 2);
    int start_y = config->center_y - (scaled_size / 2);

    ESP_LOGI(TAG, "Drawing eye at (%d, %d) with scale %d", 
             config->center_x, config->center_y, config->scale);

    for (int ey = 0; ey < EYE_SIZE; ey++) {
        for (int ex = 0; ex < EYE_SIZE; ex++) {
            uint8_t color_idx = s_eye_bitmap[ey][ex];
            uint16_t color = s_palette[color_idx];

            /* Draw scaled pixel */
            int px = start_x + (ex * config->scale);
            int py = start_y + (ey * config->scale);

            esp_err_t ret = eldra_display_fill_rect(px, py, config->scale, config->scale, color);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) {
                return ret;
            }
        }
    }

    return eldra_display_refresh();
}

esp_err_t eldra_eyes_draw_centered(int scale)
{
    if (scale < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    int display_w = eldra_display_get_width();
    int display_h = eldra_display_get_height();

    eldra_eye_config_t config = {
        .center_x = display_w / 2,
        .center_y = display_h / 2,
        .scale = scale
    };

    return eldra_eyes_draw(&config);
}

uint16_t eldra_eyes_get_palette_color(eldra_eye_color_t index)
{
    if (index >= PALETTE_SIZE) {
        return s_palette[0];
    }
    return s_palette[index];
}
