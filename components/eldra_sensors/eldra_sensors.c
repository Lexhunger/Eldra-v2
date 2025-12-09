#include "eldra_sensors.h"
#include "eldra_logging.h"

/**
 * @file eldra_sensors.c
 * @brief Sensor/peripheral bring-up wrapper using vendor drivers.
 */

#include "eldra_logging.h"
#include "BAT_Driver.h"
#include "Button_Driver.h"
#include "I2C_Driver.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "SD_MMC.h"
#include "TCA9554PWR.h"
#include "Wireless.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static eldra_sensors_imu_cb_t s_imu_cb = NULL;
static float s_battery_volts = 0.0f;
static uint8_t s_battery_percent = 0;
static uint64_t s_last_batt_log_ms = 0;
static const uint64_t BATTERY_LOG_INTERVAL_MS = 300000ULL; // 5 minutes

void eldra_sensors_set_imu_callback(eldra_sensors_imu_cb_t cb) {
    s_imu_cb = cb;
}

static uint8_t battery_voltage_to_percent(float volts) {
    const float v_full = 4.15f;
    const float v_empty = 3.30f;
    if (volts <= v_empty) {
        return 0U;
    }
    if (volts >= v_full) {
        return 100U;
    }
    float ratio = (volts - v_empty) / (v_full - v_empty);
    if (ratio < 0.0f) {
        ratio = 0.0f;
    }
    if (ratio > 1.0f) {
        ratio = 1.0f;
    }
    return (uint8_t)(ratio * 100.0f);
}

static void sd_init_task(void *param) {
    EL_LOGI("eldra_sensors", "Init: SD task start");
    SD_Init(); // vendor driver returns void; rely on its own logging if present
    EL_LOGI("eldra_sensors", "SD init attempted (no status available)");
    vTaskDelete(NULL);
}

/**
 * @brief Background task that polls the IMU/RTC/battery.
 *
 * This mirrors the vendor demo loop to keep the drivers alive until we layer
 * higher-level sensor fusion on top.
 */
static void driver_loop(void *parameter) {
    uint64_t last_us = esp_timer_get_time();
    while (1) {
        QMI8658_Loop();
        getGyroscope();
        uint64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - last_us) / 1000ULL);
        if (dt_ms == 0) {
            dt_ms = 1;
        }
        last_us = now_us;
        if (s_imu_cb) {
            s_imu_cb(Gyro.x, Gyro.y, Gyro.z, Accel.x, Accel.y, Accel.z, dt_ms);
        }
        RTC_Loop();
        float volts = BAT_Get_Volts();
        s_battery_volts = volts;
        s_battery_percent = battery_voltage_to_percent(volts);
        uint64_t now_ms = now_us / 1000ULL;
        if (now_ms - s_last_batt_log_ms >= BATTERY_LOG_INTERVAL_MS) {
            log_event(LOG_LEVEL_INFO, "battery", "battery=%.2fV (~%u%%)", volts,
                      (unsigned int)s_battery_percent);
            s_last_batt_log_ms = now_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(5)); // faster IMU poll for more responsive motion triggers (~200 Hz)
    }
    vTaskDelete(NULL);
}

esp_err_t eldra_sensors_init(void) {
    // Mirror the vendor init order so higher-level code can stay hardware-agnostic.
    EL_LOGI("eldra_sensors", "Init: buttons");
    button_Init();
    // Wireless (Wi-Fi/BLE) disabled for now to avoid unsupported BLE scan crash; re-enable when needed.
    // Wireless_Init();
    EL_LOGI("eldra_sensors", "Init: flash search");
    Flash_Searching();
    EL_LOGI("eldra_sensors", "Init: battery");
    BAT_Init();
    EL_LOGI("eldra_sensors", "Init: I2C");
    I2C_Init();
    EL_LOGI("eldra_sensors", "Init: RTC");
    PCF85063_Init();
    EL_LOGI("eldra_sensors", "Init: IMU");
    QMI8658_Init();
    EL_LOGI("eldra_sensors", "Init: IO expander");
    EXIO_Init();

    xTaskCreatePinnedToCore(
        driver_loop,
        "Other Driver task",
        4096,
        NULL,
        1, // keep priority at or below main to avoid starving app_main init
        NULL,
        0);

    EL_LOGI("eldra_sensors", "Init: SD (deferred task)");
    xTaskCreatePinnedToCore(sd_init_task, "sd_init", 3072, NULL, 3, NULL, 0);
    EL_LOGI("eldra_sensors", "Sensors init done");
    return ESP_OK;
}

float eldra_sensors_get_battery_voltage(void) {
    return s_battery_volts;
}

uint8_t eldra_sensors_get_battery_percent(void) {
    return s_battery_percent;
}

esp_err_t eldra_sensors_rtc_set(const datetime_t *time) {
    if (!time) {
        return ESP_ERR_INVALID_ARG;
    }
    PCF85063_Set_All(*time);
    char buf[64] = {0};
    datetime_to_str(buf, *time);
    log_event(LOG_LEVEL_INFO, "rtc", "RTC set to %s", buf);
    return ESP_OK;
}

esp_err_t eldra_sensors_rtc_get(datetime_t *out_time) {
    if (!out_time) {
        return ESP_ERR_INVALID_ARG;
    }
    PCF85063_Read_Time(out_time);
    return ESP_OK;
}

