#include "eldra_display_round.h"
#include "esp_log.h"

/**
 * @file eldra_display_round.c
 * @brief Implementation of the round panel wrapper that delegates to vendor drivers.
 */

#include "ST7701S.h"

esp_err_t eldra_display_round_init(void) {
    // Initialize the RGB panel (LVGL demo removed; we blit directly).
    ESP_LOGI("eldra_display_round", "LCD_Init start");
    LCD_Init();
    ESP_LOGI("eldra_display_round", "LCD_Init done (LVGL skipped; using direct blits)");
    return ESP_OK;
}

void eldra_display_round_set_backlight(uint8_t level) {
    // Pass-through to the vendor backlight driver.
    Set_Backlight(level);
}

esp_err_t eldra_display_round_reset_panel(void) {
    if (!panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_lcd_panel_reset(panel_handle);
    if (err != ESP_OK) {
        return err;
    }
    return esp_lcd_panel_init(panel_handle);
}

int eldra_display_round_get_width(void) {
    return EXAMPLE_LCD_H_RES;
}

int eldra_display_round_get_height(void) {
    return EXAMPLE_LCD_V_RES;
}

esp_err_t eldra_display_round_blit(const uint16_t *framebuffer, int width, int height) {
    if (!framebuffer || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // Draw the full frame starting at (0,0).
    return esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, width, height, framebuffer);
}
