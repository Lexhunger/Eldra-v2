#pragma once

#include "esp_err.h"
#include <time.h>

/**
 * @brief Set both date and time using format "MM/DD/YYYY HH:MM:SS AM".
 */
esp_err_t rtc_driver_set_datetime(const char *datetime_str);

/**
 * @brief Set date only, keep current time. Format: "MM/DD/YYYY".
 */
esp_err_t rtc_driver_set_date(const char *date_str);

/**
 * @brief Set time only, keep current date. Format: "HH:MM:SS AM".
 */
esp_err_t rtc_driver_set_time(const char *time_str);

/**
 * @brief Get current time in tm struct (localtime).
 */
esp_err_t rtc_driver_get_time(struct tm *out_tm);

/**
 * @brief Format current time into "MM/DD/YYYY HH:MM:SS AM" string.
 */
esp_err_t rtc_driver_format_now(char *buf, size_t len);

/**
 * @brief Persist current valid system epoch to NVS.
 *
 * Stores only if current time is valid (>= 2024-01-01 UTC).
 */
esp_err_t rtc_driver_persist_if_valid(void);

/**
 * @brief Restore system time from persisted NVS epoch when current time is invalid.
 *
 * No-op if system time is already valid.
 */
esp_err_t rtc_driver_restore_persisted(void);

