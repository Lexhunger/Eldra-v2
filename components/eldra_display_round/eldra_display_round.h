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
#include <stdbool.h>

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
 * @brief Re-assert panel runtime state (gap/orientation/controller defaults).
 *
 * Lightweight sync helper intended for troubleshooting scan-origin drift
 * without doing a full panel reset.
 */
esp_err_t eldra_display_round_sync_panel_state(void);

/**
 * @brief Get the configured panel width in pixels.
 */
int eldra_display_round_get_width(void);

/**
 * @brief Get the configured panel height in pixels.
 */
int eldra_display_round_get_height(void);

/**
 * @brief Legacy debug API kept for compatibility.
 *
 * Panel-space shifting is intentionally disabled because esp_lcd draw_bitmap
 * expects tightly packed color data for the requested window (no stride arg).
 * Use renderer-level offsets instead (eyes/glyph display center offsets).
 */
void eldra_display_round_set_shift(int dx, int dy);

/**
 * @brief Query whether the panel is ready to accept blits.
 *
 * Returns false while a reinit is in progress or before the first post-init
 * test blit has succeeded.
 */
bool eldra_display_round_is_ready(void);

/**
 * @brief Draw a simple color-bar test pattern (blocking).
 *
 * Useful for debugging panel output independent of the eyes pipeline.
 */
esp_err_t eldra_display_round_testpattern(void);

/**
 * @brief Put panel controller into/out of sleep sequence (ST7701S 28/10/11/29).
 *
 * When sleeping, blits are rejected with ESP_ERR_INVALID_STATE.
 */
esp_err_t eldra_display_round_panel_sleep(bool enable);

typedef struct {
    bool panel_ready;
    bool panel_sleeping;
    bool reinit_in_progress;
    uint32_t panel_ready_stamp_ms;
    uint32_t reset_count;
    uint32_t sync_count;
    uint32_t sleep_enter_count;
    uint32_t sleep_exit_count;
    uint32_t blit_ok_count;
    uint32_t blit_fail_count;
    uint32_t blit_timeout_count;
    uint32_t last_blit_ok_ms;
    uint32_t last_blit_fail_ms;
    esp_err_t last_blit_err;
} eldra_display_round_diag_t;

/**
 * @brief Snapshot display runtime diagnostics/counters.
 */
void eldra_display_round_get_diag(eldra_display_round_diag_t *out);

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
