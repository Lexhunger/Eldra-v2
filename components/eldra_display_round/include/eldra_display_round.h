/**
 * @file eldra_display_round.h
 * @brief Display driver for Waveshare ESP32-S3 2.8" Round Display
 */

#ifndef ELDRA_DISPLAY_ROUND_H
#define ELDRA_DISPLAY_ROUND_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Display configuration structure
 */
typedef struct {
    int width;   /**< Display width in pixels */
    int height;  /**< Display height in pixels */
} eldra_display_config_t;

/**
 * @brief Initialize the round display
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t eldra_display_init(void);

/**
 * @brief Clear the display with a specified color
 * 
 * @param color RGB565 color value
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t eldra_display_clear(uint16_t color);

/**
 * @brief Draw a pixel at specified coordinates
 * 
 * @param x X coordinate
 * @param y Y coordinate
 * @param color RGB565 color value
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t eldra_display_draw_pixel(int x, int y, uint16_t color);

/**
 * @brief Draw a filled rectangle
 * 
 * @param x X coordinate of top-left corner
 * @param y Y coordinate of top-left corner
 * @param w Width of rectangle
 * @param h Height of rectangle
 * @param color RGB565 color value
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t eldra_display_fill_rect(int x, int y, int w, int h, uint16_t color);

/**
 * @brief Get display width
 * 
 * @return Display width in pixels
 */
int eldra_display_get_width(void);

/**
 * @brief Get display height
 * 
 * @return Display height in pixels
 */
int eldra_display_get_height(void);

/**
 * @brief Get pointer to frame buffer
 * 
 * @return Pointer to RGB565 frame buffer
 */
uint16_t *eldra_display_get_framebuffer(void);

/**
 * @brief Refresh the display from frame buffer
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t eldra_display_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* ELDRA_DISPLAY_ROUND_H */
