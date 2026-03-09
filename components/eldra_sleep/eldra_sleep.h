#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "eldra_emotion.h"

#ifdef __cplusplus
extern "C" {
#endif

void eldra_sleep_init(emotion_context_t *ctx);
void eldra_sleep_sleep_now(void);
void eldra_sleep_wake_now(void);
void eldra_sleep_tick(uint64_t now_ms);
void eldra_sleep_set_glyph_offset(int dx, int dy);
void eldra_sleep_set_window(uint8_t start_hour, uint8_t end_hour);
void eldra_sleep_set_thresholds(uint32_t window_inactivity_ms,
                                uint8_t low_battery_pct,
                                uint32_t global_inactivity_ms,
                                uint32_t overfed_hold_ms);
bool eldra_sleep_on_motion(float ax_g, float ay_g, float az_g, float gx_dps, float gy_dps, float gz_dps);
bool eldra_sleep_get_shutdown_eta_ms(uint64_t now_ms, uint32_t *remaining_ms);
bool eldra_sleep_should_suppress_imu(uint64_t now_ms);
bool eldra_sleep_is_active(void);
bool eldra_sleep_is_window_now(void);

#ifdef __cplusplus
}
#endif
