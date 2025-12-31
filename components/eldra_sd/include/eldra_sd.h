#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if /sdcard is mounted.
 */
bool eldra_sd_is_mounted(void);

/**
 * @brief Wait for /sdcard to be mounted, polling until timeout_ms.
 */
esp_err_t eldra_sd_wait_for_mount(uint32_t timeout_ms, uint32_t poll_interval_ms);

/**
 * @brief Log directory entries under /sdcard (up to max_entries).
 */
void eldra_sd_log_root(int max_entries);

#ifdef __cplusplus
}
#endif
