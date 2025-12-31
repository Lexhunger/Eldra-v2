#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * @file eldra_sensors.h
 * @brief Wrapper for sensor/peripheral initialization and upkeep tasks.
 *
 * Keeps higher-level firmware agnostic of low-level driver details for IMU, RTC,
 * battery, IO expander, and buttons.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Eldra's sensor stack and supporting peripherals.
 *
 * Brings up the IO expander, battery ADC, IMU, and button handling, then
 * spawns the background loop that polls the IMU and battery at a fixed cadence.
 *
 * @return ESP_OK on success; error codes propagate from driver init.
 */
esp_err_t eldra_sensors_init(void);

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t dotw;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} datetime_t;

/**
 * @brief Optional callback to receive IMU samples (gyro dps, accel g, dt).
 *
 * Intended for the eye engine or other motion-reactive features.
 */
typedef void (*eldra_sensors_imu_cb_t)(float gx_dps,
                                       float gy_dps,
                                       float gz_dps,
                                       float ax_g,
                                       float ay_g,
                                       float az_g,
                                       uint32_t dt_ms);

/**
 * @brief Register a single IMU sample callback (overwrites previous).
 */
void eldra_sensors_set_imu_callback(eldra_sensors_imu_cb_t cb);

/**
 * @brief Retrieve the latest measured battery voltage (volts).
 */
float eldra_sensors_get_battery_voltage(void);

/**
 * @brief Retrieve the latest battery percentage (0-100).
 */
uint8_t eldra_sensors_get_battery_percent(void);

/**
 * @brief Set the system RTC datetime (backed by settimeofday()).
 * @param time Pointer to datetime to set; expected to be a full date/time.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG on bad input.
 */
esp_err_t eldra_sensors_rtc_set(const datetime_t *time);

/**
 * @brief Read the current system RTC datetime.
 * @param out_time Destination for the current datetime.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG if out_time is NULL.
 */
esp_err_t eldra_sensors_rtc_get(datetime_t *out_time);

#ifdef __cplusplus
}
#endif
