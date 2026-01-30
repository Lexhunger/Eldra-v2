#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void eldra_sleep_init(void);
void eldra_sleep_sleep_now(void);
void eldra_sleep_wake_now(void);
void eldra_sleep_tick(uint64_t now_ms);
void eldra_sleep_set_glyph_offset(int dx, int dy);

#ifdef __cplusplus
}
#endif
