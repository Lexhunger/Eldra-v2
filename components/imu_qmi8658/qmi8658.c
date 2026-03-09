#include "qmi8658.h"
#include "eldra_logging.h"

#include <stdbool.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "TCA9554PWR.h" // for shared I2C0 pins/port

#define QMI8658_ADDR_L 0x6B
#define QMI8658_ADDR_H 0x6A

#define QMI8658_WHO_AM_I 0x00
#define QMI8658_CTRL1    0x02
#define QMI8658_CTRL2    0x03
#define QMI8658_CTRL3    0x04
#define QMI8658_CTRL5    0x06
#define QMI8658_CTRL7    0x08
#define QMI8658_AX_L     0x35

typedef enum {
    ACC_RANGE_2G = 0x0,
    ACC_RANGE_4G,
    ACC_RANGE_8G,
    ACC_RANGE_16G
} acc_scale_t;

typedef enum {
    GYR_RANGE_16DPS = 0x0,
    GYR_RANGE_32DPS,
    GYR_RANGE_64DPS,
    GYR_RANGE_128DPS,
    GYR_RANGE_256DPS,
    GYR_RANGE_512DPS,
    GYR_RANGE_1024DPS
} gyro_scale_t;

// Normal mode ODRs (bits [3:0]) — we pick 250 Hz for both sensors.
typedef enum {
    ODR_8000 = 0x0,
    ODR_4000,
    ODR_2000,
    ODR_1000,
    ODR_500,
    ODR_250,
    ODR_120,
    ODR_60,
    ODR_30
} odr_t;

typedef enum {
    LPF_MODE_0 = 0x0, // 2.66% of ODR
    LPF_MODE_1 = 0x2,
    LPF_MODE_2 = 0x4,
    LPF_MODE_3 = 0x6  // 13.37% of ODR
} lpf_t;

static const char *TAG = "qmi8658";
static uint8_t s_addr = QMI8658_ADDR_L;
static bool s_inited = false;
static float s_accel_scale = 0.0f;
static float s_gyro_scale = 0.0f;

static esp_err_t i2c_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    if (len) {
        i2c_master_write(cmd, data, len, true);
    }
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t i2c_write_u8(uint8_t reg, uint8_t val)
{
    return i2c_write_reg(reg, &val, 1);
}

static esp_err_t i2c_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t probe_address(uint8_t addr)
{
    s_addr = addr;
    uint8_t who = 0;
    esp_err_t ret = i2c_read_reg(QMI8658_WHO_AM_I, &who, 1);
    if (ret == ESP_OK) {
        EL_LOGI(TAG, "Found QMI8658 at 0x%02X (WHO_AM_I=0x%02X)", addr, who);
    }
    return ret;
}

esp_err_t qmi8658_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    // Ensure the shared I2C0 bus is up (EXIO already installs it idempotently).
    ESP_RETURN_ON_ERROR(EXIO_Init(), TAG, "init EXIO/I2C failed");

    // Try low address first, then high.
    if (probe_address(QMI8658_ADDR_L) != ESP_OK) {
        ESP_RETURN_ON_ERROR(probe_address(QMI8658_ADDR_H), TAG, "probe failed");
    }

    // Configure CTRL1: enable 2 MHz oscillator (bit0=0) and auto address increment (bit6=1).
    uint8_t ctrl1 = 0x40;
    ESP_RETURN_ON_ERROR(i2c_write_u8(QMI8658_CTRL1, ctrl1), TAG, "write CTRL1 failed");

    // Set accelerometer: 4G range, 250 Hz ODR.
    uint8_t ctrl2 = (ACC_RANGE_4G << 4) | ODR_250;
    ESP_RETURN_ON_ERROR(i2c_write_u8(QMI8658_CTRL2, ctrl2), TAG, "write CTRL2 failed");
    s_accel_scale = 4.0f / 32768.0f;

    // Set gyro: 256 dps range, 250 Hz ODR.
    uint8_t ctrl3 = (GYR_RANGE_256DPS << 4) | ODR_250;
    ESP_RETURN_ON_ERROR(i2c_write_u8(QMI8658_CTRL3, ctrl3), TAG, "write CTRL3 failed");
    s_gyro_scale = 256.0f / 32768.0f;

    // Low-pass filters: enable both acc/gyro LPF with mode 3.
    uint8_t ctrl5 = 0;
    ctrl5 |= 0x01 | (LPF_MODE_3 << 1);   // acc LPF enable + mode
    ctrl5 |= 0x10 | (LPF_MODE_3 << 5);   // gyro LPF enable + mode
    ESP_RETURN_ON_ERROR(i2c_write_u8(QMI8658_CTRL5, ctrl5), TAG, "write CTRL5 failed");

    // CTRL7: enable high-speed clock + acc + gyro (full power).
    ESP_RETURN_ON_ERROR(i2c_write_u8(QMI8658_CTRL7, 0x43), TAG, "write CTRL7 failed");

    s_inited = true;
    EL_LOGI(TAG, "QMI8658 ready (addr 0x%02X)", s_addr);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t qmi8658_read_sample(qmi8658_sample_t *out)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[12];
    esp_err_t ret = i2c_read_reg(QMI8658_AX_L, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    int16_t ax = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t ay = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t az = (int16_t)((buf[5] << 8) | buf[4]);
    int16_t gx = (int16_t)((buf[7] << 8) | buf[6]);
    int16_t gy = (int16_t)((buf[9] << 8) | buf[8]);
    int16_t gz = (int16_t)((buf[11] << 8) | buf[10]);

    out->ax = ax * s_accel_scale;
    out->ay = ay * s_accel_scale;
    out->az = az * s_accel_scale;
    out->gx = gx * s_gyro_scale;
    out->gy = gy * s_gyro_scale;
    out->gz = gz * s_gyro_scale;
    return ESP_OK;
}

