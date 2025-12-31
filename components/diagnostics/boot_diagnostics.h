#pragma once

/**
 * @file boot_diagnostics.h
 * @brief Simple boot diagnostics for LCD failures (buzzer + screen pattern).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize diagnostics helpers (currently silences the buzzer).
 */
void diag_init(void);

/**
 * @brief Enter an SOS diagnostic loop when LCD init/test fails.
 *
 * @param framebuffer Optional framebuffer to draw an error pattern on.
 * @param width       Width of the framebuffer.
 * @param height      Height of the framebuffer.
 */
void diag_signal_error_lcd_init(uint16_t *framebuffer, int width, int height);

#ifdef __cplusplus
}
#endif
