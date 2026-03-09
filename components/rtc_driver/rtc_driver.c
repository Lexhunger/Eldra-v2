#include "rtc_driver.h"
#include "eldra_logging.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/i2c.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <sys/time.h>
#include "TCA9554PWR.h"

static const char *TAG = "rtc_drv";
static const char *NVS_NS = "rtc_drv";
static const char *NVS_KEY_EPOCH = "epoch";
static const int64_t VALID_EPOCH_MIN = 1704067200LL; // 2024-01-01T00:00:00Z

// External RTC (PCF85063) on the shared I2C0 bus.
// Keep these local to avoid component dependency cycles.
#define RTC_I2C_NUM              I2C_NUM_0
#define RTC_I2C_SDA              15
#define RTC_I2C_SCL              7
#define RTC_I2C_FREQ_HZ          400000
#define RTC_I2C_TIMEOUT_MS       1000
#define PCF85063_ADDR            0x51
#define PCF85063_REG_SECONDS     0x04
#define PCF85063_YEAR_OFFSET     1970
static bool s_i2c_ready = false;

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

static inline bool epoch_valid(time_t t)
{
    return ((int64_t)t >= VALID_EPOCH_MIN);
}

static uint8_t dec_to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int bcd_to_dec(uint8_t v)
{
    return ((int)((v >> 4) & 0x0F) * 10) + (int)(v & 0x0F);
}

static esp_err_t rtc_i2c_ensure_ready(void)
{
    if (s_i2c_ready) {
        return ESP_OK;
    }
    // Share the existing EXIO-managed legacy I2C bus.
    // This avoids reinstalling the driver and triggering install errors.
    esp_err_t err = EXIO_Init();
    if (err == ESP_OK) {
        s_i2c_ready = true;
    }
    return err;
}

static esp_err_t rtc_hw_write_epoch(time_t epoch_local)
{
    struct tm tm_local = {0};
    localtime_r(&epoch_local, &tm_local);

    int y = tm_local.tm_year + 1900;
    if (y < PCF85063_YEAR_OFFSET || y > 2099) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[8] = {
        PCF85063_REG_SECONDS,
        dec_to_bcd(tm_local.tm_sec),
        dec_to_bcd(tm_local.tm_min),
        dec_to_bcd(tm_local.tm_hour),
        dec_to_bcd(tm_local.tm_mday),
        dec_to_bcd(tm_local.tm_wday),
        dec_to_bcd(tm_local.tm_mon + 1),
        dec_to_bcd(y - PCF85063_YEAR_OFFSET),
    };

    ESP_RETURN_ON_ERROR(rtc_i2c_ensure_ready(), TAG, "rtc i2c init failed");
    esp_err_t err = i2c_master_write_to_device(RTC_I2C_NUM, PCF85063_ADDR,
                                               payload, sizeof(payload),
                                               RTC_I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    return err;
}

static esp_err_t rtc_hw_read_epoch(time_t *epoch_local_out)
{
    if (!epoch_local_out) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(rtc_i2c_ensure_ready(), TAG, "rtc i2c init failed");

    uint8_t reg = PCF85063_REG_SECONDS;
    uint8_t buf[7] = {0};
    esp_err_t err = i2c_master_write_read_device(RTC_I2C_NUM, PCF85063_ADDR,
                                                 &reg, 1, buf, sizeof(buf),
                                                 RTC_I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        return err;
    }

    struct tm tm_local = {0};
    tm_local.tm_sec  = bcd_to_dec(buf[0] & 0x7F);
    tm_local.tm_min  = bcd_to_dec(buf[1] & 0x7F);
    tm_local.tm_hour = bcd_to_dec(buf[2] & 0x3F);
    tm_local.tm_mday = bcd_to_dec(buf[3] & 0x3F);
    tm_local.tm_wday = bcd_to_dec(buf[4] & 0x07);
    tm_local.tm_mon  = bcd_to_dec(buf[5] & 0x1F) - 1;
    tm_local.tm_year = (bcd_to_dec(buf[6]) + PCF85063_YEAR_OFFSET) - 1900;

    if (tm_local.tm_mon < 0 || tm_local.tm_mon > 11 || tm_local.tm_mday < 1 || tm_local.tm_mday > 31) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    time_t epoch_local = mktime(&tm_local);
    if (epoch_local == (time_t)-1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *epoch_local_out = epoch_local;
    return ESP_OK;
}

static esp_err_t persist_epoch(time_t epoch)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i64(h, NVS_KEY_EPOCH, (int64_t)epoch);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
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
    (void)rtc_driver_persist_if_valid();
    EL_LOGI(TAG, "RTC set to %s", datetime_str);
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
    (void)rtc_driver_persist_if_valid();
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
    (void)rtc_driver_persist_if_valid();
    return ESP_OK;
}

esp_err_t rtc_driver_get_time(struct tm *out_tm)
{
    if (!out_tm) return ESP_ERR_INVALID_ARG;

    struct timeval now_try = {0};
    gettimeofday(&now_try, NULL);
    if (!epoch_valid(now_try.tv_sec)) {
        time_t hw_epoch = 0;
        if (rtc_hw_read_epoch(&hw_epoch) == ESP_OK && epoch_valid(hw_epoch)) {
            struct timeval tv = {
                .tv_sec = hw_epoch,
                .tv_usec = 0,
            };
            (void)settimeofday(&tv, NULL);
            // Keep NVS mirror in sync after successful external restore.
            (void)persist_epoch(hw_epoch);
            EL_LOGI(TAG, "Restored system time from external RTC epoch=%lld", (long long)hw_epoch);
        }
    }

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

esp_err_t rtc_driver_persist_if_valid(void)
{
    time_t now_epoch = time(NULL);
    if (!epoch_valid(now_epoch)) {
        return ESP_ERR_INVALID_STATE;
    }
    // Best effort to keep external RTC in sync; still persist in NVS.
    (void)rtc_hw_write_epoch(now_epoch);
    return persist_epoch(now_epoch);
}

esp_err_t rtc_driver_restore_persisted(void)
{
    time_t now_epoch = time(NULL);
    if (epoch_valid(now_epoch)) {
        return ESP_OK; // Already valid; do not overwrite.
    }

    // Prefer external RTC if available.
    time_t hw_epoch = 0;
    if (rtc_hw_read_epoch(&hw_epoch) == ESP_OK && epoch_valid(hw_epoch)) {
        struct timeval tv_hw = {
            .tv_sec = hw_epoch,
            .tv_usec = 0,
        };
        if (settimeofday(&tv_hw, NULL) == 0) {
            (void)persist_epoch(hw_epoch);
            EL_LOGI(TAG, "Restored system time from external RTC epoch=%lld", (long long)hw_epoch);
            return ESP_OK;
        }
    }

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    int64_t saved_epoch = 0;
    err = nvs_get_i64(h, NVS_KEY_EPOCH, &saved_epoch);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    if (saved_epoch < VALID_EPOCH_MIN) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    struct timeval tv = {
        .tv_sec = (time_t)saved_epoch,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    EL_LOGI(TAG, "Restored system time from NVS epoch=%lld", (long long)saved_epoch);
    return ESP_OK;
}

