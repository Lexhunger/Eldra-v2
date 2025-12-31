#pragma once

#include "esp_err.h"

typedef struct {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
} qmi8658_sample_t;

// Initialize the QMI8658 6-axis IMU on I2C0 (SDA=15, SCL=7). Safe to call repeatedly.
esp_err_t qmi8658_init(void);

// Read one accelerometer/gyro sample (g and dps). Requires qmi8658_init() first.
esp_err_t qmi8658_read_sample(qmi8658_sample_t *out);
