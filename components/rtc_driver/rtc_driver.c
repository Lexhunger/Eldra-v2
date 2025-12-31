#include "rtc_driver.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include <sys/time.h>

static const char *TAG = "rtc_drv";

static bool parse_date(const char *s, struct tm *tm_out)
{
    int m, d, y;
    if (sscanf(s, "%d/%d/%d", &m, &d, &y) != 3) {
        return false;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1970 || y > 2099) {
        return false;
    }
    tm_out->tm_year = y - 1900;
    tm_out->tm_mon = m - 1;
    tm_out->tm_mday = d;
    return true;
}

static bool parse_time_ampm(const char *s, struct tm *tm_out)
{
    int h, min, sec;
    char ampm[3] = {0};
    if (sscanf(s, "%d:%d:%d %2s", &h, &min, &sec, ampm) != 4) {
        return false;
    }
    for (int i = 0; i < 2; i++) {
        ampm[i] = (char)toupper((unsigned char)ampm[i]);
    }
    if (h < 1 || h > 12 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        return false;
    }
    int is_pm = (strcmp(ampm, "PM") == 0);
    if (strcmp(ampm, "AM") != 0 && !is_pm) {
        return false;
    }
    if (h == 12) {
        h = 0;
    }
    h += is_pm ? 12 : 0;
    tm_out->tm_hour = h;
    tm_out->tm_min = min;
    tm_out->tm_sec = sec;
    return true;
}

static esp_err_t apply_tm(struct tm *tm_new)
{
    time_t t = mktime(tm_new);
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
    return ESP_OK;
}

esp_err_t rtc_driver_set_datetime(const char *datetime_str)
{
    if (!datetime_str) {
        return ESP_ERR_INVALID_ARG;
    }
    // Expected "MM/DD/YYYY HH:MM:SS AM"
    char date[16] = {0};
    char time[16] = {0};
    if (sscanf(datetime_str, "%15s %15[^\n]", date, time) != 2) {
        return ESP_ERR_INVALID_ARG;
    }
    struct tm tm_new = {0};
    if (!parse_date(date, &tm_new) || !parse_time_ampm(time, &tm_new)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(apply_tm(&tm_new), TAG, "set datetime failed");
    ESP_LOGI(TAG, "RTC set to %s", datetime_str);
    return ESP_OK;
}

esp_err_t rtc_driver_set_date(const char *date_str)
{
    if (!date_str) return ESP_ERR_INVALID_ARG;
    struct timeval now;
    gettimeofday(&now, NULL);
    struct tm tm_cur;
    localtime_r(&now.tv_sec, &tm_cur);

    if (!parse_date(date_str, &tm_cur)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(apply_tm(&tm_cur), TAG, "set date failed");
    return ESP_OK;
}

esp_err_t rtc_driver_set_time(const char *time_str)
{
    if (!time_str) return ESP_ERR_INVALID_ARG;
    struct timeval now;
    gettimeofday(&now, NULL);
    struct tm tm_cur;
    localtime_r(&now.tv_sec, &tm_cur);

    if (!parse_time_ampm(time_str, &tm_cur)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(apply_tm(&tm_cur), TAG, "set time failed");
    return ESP_OK;
}

esp_err_t rtc_driver_get_time(struct tm *out_tm)
{
    if (!out_tm) return ESP_ERR_INVALID_ARG;
    struct timeval now;
    gettimeofday(&now, NULL);
    localtime_r(&now.tv_sec, out_tm);
    return ESP_OK;
}

esp_err_t rtc_driver_format_now(char *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    struct tm tm_now;
    ESP_RETURN_ON_ERROR(rtc_driver_get_time(&tm_now), TAG, "get time failed");
    int hour12 = tm_now.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char *ampm = (tm_now.tm_hour >= 12) ? "PM" : "AM";
    snprintf(buf, len, "%02d/%02d/%04d %02d:%02d:%02d %s",
             tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_year + 1900,
             hour12, tm_now.tm_min, tm_now.tm_sec, ampm);
    return ESP_OK;
}
