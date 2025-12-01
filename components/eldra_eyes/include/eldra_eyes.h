/**
 * @file eldra_eyes.h
 * @brief Eye rendering module with 11x11 pixel art eye
 */

#ifndef ELDRA_EYES_H
#define ELDRA_EYES_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 8-color RGB565 palette indices
 */
typedef enum {
    EYE_COLOR_BLACK = 0,
    EYE_COLOR_WHITE,
    EYE_COLOR_BLUE,
    EYE_COLOR_RED,
    EYE_COLOR_GREEN,
    EYE_COLOR_YELLOW,
    EYE_COLOR_CYAN,
    EYE_COLOR_MAGENTA
} eldra_eye_color_t;

/**
 * @brief Eye configuration structure
 */
typedef struct {
    int center_x;    /**< X center position on display */
    int center_y;    /**< Y center position on display */
    int scale;       /**< Scale factor (1 = 11x11, 2 = 22x22, etc.) */
} eldra_eye_config_t;

/**
 * @brief Initialize the eyes module
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t eldra_eyes_init(void);

/**
 * @brief Draw an eye at specified position with scale
 * 
 * @param config Eye configuration (position and scale)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t eldra_eyes_draw(const eldra_eye_config_t *config);

/**
 * @brief Draw eye centered on display with specified scale
 * 
 * @param scale Scale factor for the eye
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t eldra_eyes_draw_centered(int scale);

/**
 * @brief Get RGB565 color from palette index
 * 
 * @param index Palette index (0-7)
 * @return RGB565 color value
 */
uint16_t eldra_eyes_get_palette_color(eldra_eye_color_t index);

#ifdef __cplusplus
}
#endif

#endif /* ELDRA_EYES_H */
