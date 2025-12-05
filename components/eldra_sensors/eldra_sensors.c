#include "eldra_sensors.h"
#include "esp_log.h"
#include "eldra_sensors.h"

/**
 * @file eldra_sensors.c
 * @brief Sensor/peripheral bring-up wrapper using vendor drivers.
 */

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

void eldra_sensors_set_imu_callback(eldra_sensors_imu_cb_t cb) {
    s_imu_cb = cb;
}

static void sd_init_task(void *param) {
    ESP_LOGI("eldra_sensors", "Init: SD task start");
    SD_Init(); // vendor driver returns void; rely on its own logging if present
    ESP_LOGI("eldra_sensors", "SD init attempted (no status available)");
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
        BAT_Get_Volts();
        vTaskDelay(pdMS_TO_TICKS(5)); // faster IMU poll for more responsive motion triggers (~200 Hz)
    }
    vTaskDelete(NULL);
}

esp_err_t eldra_sensors_init(void) {
    // Mirror the vendor init order so higher-level code can stay hardware-agnostic.
    ESP_LOGI("eldra_sensors", "Init: buttons");
    button_Init();
    // Wireless (Wi-Fi/BLE) disabled for now to avoid unsupported BLE scan crash; re-enable when needed.
    // Wireless_Init();
    ESP_LOGI("eldra_sensors", "Init: flash search");
    Flash_Searching();
    ESP_LOGI("eldra_sensors", "Init: battery");
    BAT_Init();
    ESP_LOGI("eldra_sensors", "Init: I2C");
    I2C_Init();
    ESP_LOGI("eldra_sensors", "Init: RTC");
    PCF85063_Init();
    ESP_LOGI("eldra_sensors", "Init: IMU");
    QMI8658_Init();
    ESP_LOGI("eldra_sensors", "Init: IO expander");
    EXIO_Init();

    xTaskCreatePinnedToCore(
        driver_loop,
        "Other Driver task",
        4096,
        NULL,
        1, // keep priority at or below main to avoid starving app_main init
        NULL,
        0);

    ESP_LOGI("eldra_sensors", "Init: SD (deferred task)");
    xTaskCreatePinnedToCore(sd_init_task, "sd_init", 3072, NULL, 3, NULL, 0);
    ESP_LOGI("eldra_sensors", "Sensors init done");
    return ESP_OK;
}
