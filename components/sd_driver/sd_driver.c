#include "sd_driver.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>
#include <inttypes.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "TCA9554PWR.h"

#define SD_MOUNT_POINT "/sdcard"
#define SD_LOG_DIR     SD_MOUNT_POINT "/LOGS"
#define SD_WIFI_DIR    SD_MOUNT_POINT "/WIFI"
#define SD_WIFI_FILE   SD_WIFI_DIR "/WIFI.CFG"
#define SD_CONFIG_DIR  SD_MOUNT_POINT "/CONFIG"

// Board wiring from vendor demo (1-bit SDMMC)
#define SD_PIN_CLK  2
#define SD_PIN_CMD  1
#define SD_PIN_D0   42
#define SD_PIN_D1   -1
#define SD_PIN_D2   -1
#define SD_PIN_D3   -1 // D3 enable via EXIO4

static const char *TAG = "sd_drv";

typedef enum {
    SD_OP_LIST,
    SD_OP_READ,
    SD_OP_DELETE,
    SD_OP_LOG,
    SD_OP_WIFI_SAVE,
} sd_op_t;

typedef struct {
    sd_op_t op;
    char path[128];
    char data[256];
    char extra[128];
} sd_job_t;

static QueueHandle_t sd_queue = NULL;
static TaskHandle_t sd_task_handle = NULL;
static bool sd_mounted = false;
static sdmmc_card_t *sd_card = NULL;

static esp_err_t sd_mount(void);
static void sd_worker(void *arg);

static void ensure_dirs(const char *path)
{
    // Minimal mkdir -p: walk path and create segments.
    char tmp[160];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
    mkdir(tmp, 0775);
}

static esp_err_t sd_mount(void)
{
    if (sd_mounted) {
        return ESP_OK;
    }

    // Enable EXIO D3 (board uses IO expander for SD D3/power rail)
    Set_EXIO(TCA9554_EXIO4, true);
    vTaskDelay(pdMS_TO_TICKS(10));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; // 1-bit bus
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.d1 = SD_PIN_D1;
    slot_config.d2 = SD_PIN_D2;
    slot_config.d3 = SD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    sd_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, sd_card);

    ensure_dirs(SD_LOG_DIR);
    ensure_dirs(SD_WIFI_DIR);
    ensure_dirs(SD_CONFIG_DIR);
    return ESP_OK;
}

esp_err_t sd_driver_format(void)
{
    ESP_RETURN_ON_ERROR(sd_mount(), TAG, "mount before format");
    ESP_LOGW(TAG, "Formatting SD card (destructive)...");
    esp_err_t ret = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card);
    sd_mounted = false;
    sd_card = NULL;
    ESP_RETURN_ON_ERROR(sd_mount(), TAG, "remount after format");
    ESP_LOGI(TAG, "Format complete and remounted");
    return ESP_OK;
}

static void sd_log_rotate(void)
{
    DIR *dir = opendir(SD_LOG_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Open dir failed (%s)", strerror(errno));
        return;
    }
    char names[16][64];
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < 16) {
        if (ent->d_type == DT_DIR) {
            continue;
        }
        const char *n = ent->d_name;
        size_t len = strlen(n);
        if (len > 4 && strcmp(n + len - 4, ".log") == 0) {
            strlcpy(names[count++], n, sizeof(names[0]));
        }
    }
    closedir(dir);
    if (count <= 5) {
        return;
    }
    // sort lexicographically (YYYYMMDD.log order)
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                char tmp[64];
                memcpy(tmp, names[i], sizeof(tmp));
                memcpy(names[i], names[j], sizeof(names[i]));
                memcpy(names[j], tmp, sizeof(names[j]));
            }
        }
    }
    for (int i = 0; i < count - 5; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", SD_LOG_DIR, names[i]);
        unlink(path);
        ESP_LOGI(TAG, "Rotated old log %s", names[i]);
    }
}

static void make_date_stamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm) == NULL || tm.tm_year + 1900 < 2020) {
        // Fallback: use uptime days
        uint32_t days = (uint32_t)(esp_timer_get_time() / (1000000ULL * 86400ULL));
        snprintf(buf, len, "uptime%03" PRIu32, days);
    } else {
        snprintf(buf, len, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    }
}

static void handle_log(const char *line)
{
    if (!line) return;
    ensure_dirs(SD_LOG_DIR);
    char date[16];
    make_date_stamp(date, sizeof(date));
    char path[192];
    snprintf(path, sizeof(path), "%s/%s.log", SD_LOG_DIR, date);
    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Open log failed (%s)", strerror(errno));
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(f, "[%02d:%02d:%02d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    fclose(f);
    sd_log_rotate();
}

static void handle_list(const char *path)
{
    const char *p = (path && path[0]) ? path : SD_MOUNT_POINT;
    DIR *dir = opendir(p);
    if (!dir) {
        printf("SD list: failed to open %s (%s)\n", p, strerror(errno));
        return;
    }
    printf("Listing %s\n", p);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        printf("  %s\n", ent->d_name);
    }
    closedir(dir);
}

static void handle_read(const char *path)
{
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("SD read: failed to open %s (%s)\n", path, strerror(errno));
        return;
    }
    printf("---- %s ----\n", path);
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    printf("\n---- end ----\n");
    fclose(f);
}

static void handle_delete(const char *path)
{
    if (!path) return;
    int r = unlink(path);
    if (r == 0) {
        printf("Deleted %s\n", path);
    } else {
        printf("Delete failed %s (%s)\n", path, strerror(errno));
    }
}

static void handle_wifi_save(const char *ssid, const char *pass)
{
    ensure_dirs(SD_WIFI_DIR);
    FILE *f = fopen(SD_WIFI_FILE, "w");
    if (!f) {
        printf("WiFi save failed (%s)\n", strerror(errno));
        return;
    }
    fprintf(f, "ssid=%s\npass=%s\n", ssid, pass);
    fclose(f);
    printf("WiFi credentials saved to %s\n", SD_WIFI_FILE);
}

static void sd_worker(void *arg)
{
    (void)arg;
    while (1) {
        sd_job_t job;
        if (xQueueReceive(sd_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!sd_mounted) {
            if (sd_mount() != ESP_OK) {
                printf("SD not mounted, skipping job\n");
                continue;
            }
        }
        switch (job.op) {
        case SD_OP_LIST:
            handle_list(job.path);
            break;
        case SD_OP_READ:
            handle_read(job.path);
            break;
        case SD_OP_DELETE:
            handle_delete(job.path);
            break;
        case SD_OP_LOG:
            handle_log(job.data);
            break;
        case SD_OP_WIFI_SAVE:
            handle_wifi_save(job.path, job.data);
            break;
        default:
            break;
        }
    }
}

esp_err_t sd_driver_init(void)
{
    if (sd_queue && sd_task_handle) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(sd_mount(), TAG, "sd mount");

    sd_queue = xQueueCreate(8, sizeof(sd_job_t));
    if (!sd_queue) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(sd_worker, "sd_worker", 4096, NULL, 3, &sd_task_handle, 1);
    if (ok != pdPASS) {
        vQueueDelete(sd_queue);
        sd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "SD driver ready");
    return ESP_OK;
}

static esp_err_t enqueue_job(const sd_job_t *job)
{
    if (!sd_queue) {
        esp_err_t ret = sd_driver_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (xQueueSend(sd_queue, job, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t sd_driver_list_async(const char *path)
{
    sd_job_t job = {0};
    job.op = SD_OP_LIST;
    if (path) {
        strlcpy(job.path, path, sizeof(job.path));
    } else {
        strlcpy(job.path, SD_MOUNT_POINT, sizeof(job.path));
    }
    return enqueue_job(&job);
}

esp_err_t sd_driver_read_async(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    sd_job_t job = {0};
    job.op = SD_OP_READ;
    strlcpy(job.path, path, sizeof(job.path));
    return enqueue_job(&job);
}

esp_err_t sd_driver_delete_async(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    sd_job_t job = {0};
    job.op = SD_OP_DELETE;
    strlcpy(job.path, path, sizeof(job.path));
    return enqueue_job(&job);
}

esp_err_t sd_driver_log_async(const char *line)
{
    if (!line) return ESP_ERR_INVALID_ARG;
    sd_job_t job = {0};
    job.op = SD_OP_LOG;
    strlcpy(job.data, line, sizeof(job.data));
    return enqueue_job(&job);
}

esp_err_t sd_driver_save_wifi_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }
    sd_job_t job = {0};
    job.op = SD_OP_WIFI_SAVE;
    strlcpy(job.path, ssid, sizeof(job.path));
    strlcpy(job.data, password, sizeof(job.data));
    return enqueue_job(&job);
}
