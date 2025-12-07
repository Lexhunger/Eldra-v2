#pragma once

/**
 * @file backlight.h
 * @brief Centralized backlight control for the Waveshare ESP32-S3-LCD-2.8C.
 *
 * All backlight access goes through this module to keep startup current low
 * (dark boot -> LCD init -> ramp to target brightness).
 */

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the backlight pin as GPIO output and force it low.
 *
 * Intended to be called as early as possible in app_main to avoid inrush
 * while other subsystems power up.
 */
void backlight_init_off(void);

/**
 * @brief Configure LEDC for PWM on the backlight pin (starts at 0% duty).
 */
void backlight_setup_pwm(void);

/**
 * @brief Set the raw LEDC duty (0-1023 for 10-bit resolution).
 */
void backlight_set_raw(uint16_t duty);

/**
 * @brief Set brightness using percentage scaling.
 *
 * 100% maps to CONFIG_BACKLIGHT_MAX_RAW_AT_FULL_PERCENT (not necessarily the
 * LEDC max) to keep default brightness battery friendly.
 */
void backlight_set_brightness_percent(uint8_t percent);

/**
 * @brief Ramp smoothly from the current brightness to target_percent.
 *
 * @param target_percent 0-100% brightness target.
 * @param step_ms        Delay between ramp steps.
 */
void backlight_ramp_to_percent(uint8_t target_percent, uint16_t step_ms);

/**
 * @brief Query the current raw LEDC duty cached by the driver.
 */
uint16_t backlight_get_raw(void);

#ifdef __cplusplus
}
#endif
