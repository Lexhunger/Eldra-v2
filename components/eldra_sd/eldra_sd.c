#include "eldra_sd.h"

#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "eldra_logging.h"

#define TAG "eldra_sd"

bool eldra_sd_is_mounted(void)
{
    struct stat st;
    return stat("/sdcard", &st) == 0;
}

esp_err_t eldra_sd_wait_for_mount(uint32_t timeout_ms, uint32_t poll_interval_ms)
{
    uint32_t waited = 0;
    uint32_t interval = (poll_interval_ms == 0) ? 250 : poll_interval_ms;
    while (waited < timeout_ms) {
        if (eldra_sd_is_mounted()) {
            EL_LOGI(TAG, "SD card mounted");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(interval));
        waited += interval;
    }
    EL_LOGE(TAG, "SD card not mounted after %u ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

void eldra_sd_log_root(int max_entries)
{
    DIR *dir = opendir("/sdcard");
    if (!dir) {
        EL_LOGE(TAG, "Failed to open /sdcard for listing");
        return;
    }
    if (max_entries <= 0) {
        max_entries = 20;
    }
    EL_LOGI(TAG, "Listing /sdcard:");
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL && count < max_entries) {
        EL_LOGI(TAG, "  %s", ent->d_name);
        count++;
    }
    if (count == 0) {
        EL_LOGI(TAG, "  (empty)");
    }
    closedir(dir);
}
