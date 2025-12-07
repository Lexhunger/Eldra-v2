#pragma once

/**
 * @file eldra_display_round.h
 * @brief Display wrapper for the 480x480 RGB round panel (direct blit).
 *
 * Provides panel initialization, backlight control, and dimension queries while
 * hiding vendor-specific details from the rest of the firmware. LVGL is not used.
 */

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the round RGB panel.
 *
 * Wraps the vendor LCD bring-up (LCD_Init) so the rest of the
 * firmware does not depend on panel-specific details.
 *
 * @return ESP_OK on success; error codes propagate from vendor drivers.
 */
esp_err_t eldra_display_round_init(void);

/**
 * @brief Set the backlight brightness.
 *
 * Pass-through to the vendor driver Set_Backlight.
 *
 * @param level Brightness 0-100 (clamped by the vendor driver if out of range).
 */
void eldra_display_round_set_backlight(uint8_t level);

/**
 * @brief Reset and re-init the panel (RGB driver only; does not rerun ST7701S_screen_init).
 *
 * Useful for retrying bring-up if the first init occurred during a brownout.
 *
 * @return ESP_OK on success; error codes from esp_lcd_panel_reset/init otherwise.
 */
esp_err_t eldra_display_round_reset_panel(void);

/**
 * @brief Get the configured panel width in pixels.
 */
int eldra_display_round_get_width(void);

/**
 * @brief Get the configured panel height in pixels.
 */
int eldra_display_round_get_height(void);

/**
 * @brief Push an RGB565 framebuffer to the panel.
 *
 * @param framebuffer Pointer to RGB565 pixel data.
 * @param width       Width in pixels.
 * @param height      Height in pixels.
 *
 * @return ESP_OK on success; error codes from esp_lcd_panel_draw_bitmap otherwise.
 */
esp_err_t eldra_display_round_blit(const uint16_t *framebuffer, int width, int height);

#ifdef __cplusplus
}
#endif
