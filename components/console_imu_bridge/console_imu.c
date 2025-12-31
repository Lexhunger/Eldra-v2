#include "console_imu.h"

#include "esp_console.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmi8658.h"

static const char *TAG = "console_imu";

static int cmd_imu_init(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t ret = qmi8658_init();
    if (ret != ESP_OK) {
        printf("IMU init failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("IMU init OK\n");
    }
    return 0;
}

static int cmd_imu_read(int argc, char **argv)
{
    (void)argc; (void)argv;
    qmi8658_sample_t s = {0};
    esp_err_t ret = qmi8658_read_sample(&s);
    if (ret != ESP_OK) {
        printf("IMU read failed: %s\n", esp_err_to_name(ret));
        return 0;
    }
    printf("Accel[g]: X=%0.3f Y=%0.3f Z=%0.3f | Gyro[dps]: X=%0.2f Y=%0.2f Z=%0.2f\n",
           s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
    return 0;
}

esp_err_t ConsoleIMU_Init(void)
{
    const esp_console_cmd_t init_cmd = {
        .command = "imu_init",
        .help = "Initialize QMI8658 6-axis IMU",
        .hint = NULL,
        .func = &cmd_imu_init,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&init_cmd), TAG, "register imu_init failed");

    const esp_console_cmd_t read_cmd = {
        .command = "imu_read",
        .help = "Read one IMU sample (accel[g], gyro[dps])",
        .hint = NULL,
        .func = &cmd_imu_read,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&read_cmd), TAG, "register imu_read failed");

    ESP_LOGI(TAG, "IMU console commands ready");
    return ESP_OK;
}
