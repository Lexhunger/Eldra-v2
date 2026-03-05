#include "console_rtc.h"
#include "eldra_logging.h"

#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_check.h"
#include "esp_log.h"
#include "rtc_driver.h"

static const char *TAG = "console_rtc";

static int cmd_rtc_get(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[64];
    esp_err_t ret = rtc_driver_format_now(buf, sizeof(buf));
    if (ret != ESP_OK) {
        printf("RTC get failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("RTC: %s\n", buf);
    }
    return 0;
}

static int cmd_rtc_set_datetime(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: rtc_set_datetime \"MM/DD/YYYY\" \"HH:MM:SS AM\"\n");
        return 0;
    }
    char combined[64];
    snprintf(combined, sizeof(combined), "%s %s", argv[1], argv[2]);
    esp_err_t ret = rtc_driver_set_datetime(combined);
    if (ret != ESP_OK) {
        printf("Set datetime failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("RTC set to %s\n", combined);
    }
    return 0;
}

static int cmd_rtc_set_date(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: rtc_set_date MM/DD/YYYY\n");
        return 0;
    }
    esp_err_t ret = rtc_driver_set_date(argv[1]);
    if (ret != ESP_OK) {
        printf("Set date failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("Date set\n");
    }
    return 0;
}

static int cmd_rtc_set_time(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: rtc_set_time \"HH:MM:SS AM\"\n");
        return 0;
    }
    esp_err_t ret = rtc_driver_set_time(argv[1]);
    if (ret != ESP_OK) {
        printf("Set time failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("Time set\n");
    }
    return 0;
}

esp_err_t ConsoleRTC_Init(void)
{
    const esp_console_cmd_t get_cmd = {
        .command = "rtc_get",
        .help = "Show current date/time",
        .hint = NULL,
        .func = &cmd_rtc_get,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&get_cmd), TAG, "register rtc_get failed");

    const esp_console_cmd_t set_dt_cmd = {
        .command = "rtc_set_datetime",
        .help = "Set date/time. Usage: rtc_set_datetime \"MM/DD/YYYY\" \"HH:MM:SS AM\"",
        .hint = NULL,
        .func = &cmd_rtc_set_datetime,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&set_dt_cmd), TAG, "register rtc_set_datetime failed");

    const esp_console_cmd_t set_d_cmd = {
        .command = "rtc_set_date",
        .help = "Set date only. Usage: rtc_set_date MM/DD/YYYY",
        .hint = NULL,
        .func = &cmd_rtc_set_date,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&set_d_cmd), TAG, "register rtc_set_date failed");

    const esp_console_cmd_t set_t_cmd = {
        .command = "rtc_set_time",
        .help = "Set time only. Usage: rtc_set_time \"HH:MM:SS AM\"",
        .hint = NULL,
        .func = &cmd_rtc_set_time,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&set_t_cmd), TAG, "register rtc_set_time failed");

    EL_LOGI(TAG, "RTC console commands ready");
    return ESP_OK;
}

