#include "eldra_sensors.h"
#include "eldra_logging.h"

/**
 * @file eldra_sensors.c
 * @brief Sensor/peripheral bring-up wrapper using the tested driver stack from the console demo.
 */

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "BAT_Driver.h"
#include "Button_Driver.h"
#include "TCA9554PWR.h"
#include "imu_qmi8658.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eldra_sensors";
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

static void imu_poll_task(void *arg) {
    (void)arg;
    uint64_t last_us = esp_timer_get_time();
    const TickType_t delay_ticks = pdMS_TO_TICKS(10); // ~100 Hz

    while (1) {
        qmi8658_sample_t sample = {0};
        esp_err_t ret = qmi8658_read_sample(&sample);

        uint64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - last_us) / 1000ULL);
        if (dt_ms == 0) {
            dt_ms = 1;
        }
        last_us = now_us;

        if (ret == ESP_OK && s_imu_cb) {
            s_imu_cb(sample.gx, sample.gy, sample.gz, sample.ax, sample.ay, sample.az, dt_ms);
        }

        float volts = BAT_Get_Volts();
        s_battery_volts = volts;
        s_battery_percent = battery_voltage_to_percent(volts);
        uint64_t now_ms = now_us / 1000ULL;
        if (now_ms - s_last_batt_log_ms >= BATTERY_LOG_INTERVAL_MS) {
            log_event(LOG_LEVEL_INFO, "battery", "battery=%.2fV (~%u%%)", volts,
                      (unsigned int)s_battery_percent);
            s_last_batt_log_ms = now_ms;
        }

        vTaskDelay(delay_ticks);
    }
}

static void tm_from_datetime(const datetime_t *src, struct tm *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->tm_year = (int)src->year - 1900;
    dst->tm_mon = (int)src->month - 1;
    dst->tm_mday = (int)src->day;
    dst->tm_hour = (int)src->hour;
    dst->tm_min = (int)src->minute;
    dst->tm_sec = (int)src->second;
}

esp_err_t eldra_sensors_init(void) {
    EL_LOGI(TAG, "Init: IO expander (EXIO)");
    ESP_RETURN_ON_ERROR(EXIO_Init(), TAG, "EXIO init failed");

    EL_LOGI(TAG, "Init: buttons");
    button_Init();

    EL_LOGI(TAG, "Init: battery ADC");
    BAT_Init();

    EL_LOGI(TAG, "Init: IMU (QMI8658)");
    esp_err_t imu_ret = qmi8658_init();
    if (imu_ret != ESP_OK) {
        EL_LOGW(TAG, "IMU init failed: %s", esp_err_to_name(imu_ret));
    }

    if (xTaskCreatePinnedToCore(imu_poll_task, "imu_poll", 4096, NULL, 2, NULL, 0) != pdPASS) {
        EL_LOGE(TAG, "Failed to create IMU poll task");
        return ESP_ERR_NO_MEM;
    }

    EL_LOGI(TAG, "Sensors init done");
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
    struct tm tm_new;
    tm_from_datetime(time, &tm_new);
    time_t t = mktime(&tm_new);
    if (t == (time_t)-1) {
        return ESP_ERR_INVALID_ARG;
    }
    struct timeval tv = {
        .tv_sec = t,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    char buf[64];
    strftime(buf, sizeof(buf), "%m/%d/%Y %I:%M:%S %p", &tm_new);
    log_event(LOG_LEVEL_INFO, "rtc", "RTC set to %s", buf);
    return ESP_OK;
}

esp_err_t eldra_sensors_rtc_get(datetime_t *out_time) {
    if (!out_time) {
        return ESP_ERR_INVALID_ARG;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_now;
    localtime_r(&tv.tv_sec, &tm_now);
    out_time->year = (uint16_t)(tm_now.tm_year + 1900);
    out_time->month = (uint8_t)(tm_now.tm_mon + 1);
    out_time->day = (uint8_t)tm_now.tm_mday;
    out_time->dotw = (uint8_t)tm_now.tm_wday;
    out_time->hour = (uint8_t)tm_now.tm_hour;
    out_time->minute = (uint8_t)tm_now.tm_min;
    out_time->second = (uint8_t)tm_now.tm_sec;
    return ESP_OK;
}
