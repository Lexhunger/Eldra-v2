#pragma once

#include "esp_err.h"
#include "PCF85063.h"

/**
 * @file eldra_sensors.h
 * @brief Wrapper for sensor/peripheral initialization and upkeep tasks.
 *
 * Keeps higher-level firmware agnostic of vendor driver details for IMU, RTC,
 * battery, SD, wireless, IO expander, and buttons.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Eldra’s sensor stack and supporting peripherals.
 *
 * Wraps the vendor bring-up sequence (I2C, IO expander, battery, RTC,
 * IMU, SD, wireless, buttons) and spawns the background loop that polls
 * sensors at a fixed cadence. Higher layers stay hardware-agnostic.
 *
 * @return ESP_OK on success; error codes propagate from vendor drivers.
 */
esp_err_t eldra_sensors_init(void);

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
 * @brief Set the RTC datetime using the PCF85063 driver.
 * @param time Pointer to datetime to set; expected to be a full date/time.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG on bad input.
 */
esp_err_t eldra_sensors_rtc_set(const datetime_t *time);

/**
 * @brief Read the current RTC datetime.
 * @param out_time Destination for the current datetime.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG if out_time is NULL.
 */
esp_err_t eldra_sensors_rtc_get(datetime_t *out_time);

#ifdef __cplusplus
}
#endif
