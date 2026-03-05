#include "job_queue.h"
#include "eldra_logging.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "LVGL_Driver.h"
#include "ST7701S.h"

static const char *TAG = "jobs";
static QueueHandle_t job_q = NULL;
static TaskHandle_t job_task = NULL;

static void job_worker(void *arg)
{
    job_t job;
    while (xQueueReceive(job_q, &job, portMAX_DELAY) == pdTRUE) {
        switch (job.type) {
            case JOB_SAY:
                LVGL_SendText(job.text);
                break;
            case JOB_CLEAR:
                LCD_Clear(job.color);
                break;
            default:
                EL_LOGW(TAG, "Unknown job type %d", job.type);
                break;
        }
    }
}

esp_err_t job_queue_init(void)
{
    if (job_q) {
        return ESP_OK;
    }
    job_q = xQueueCreate(8, sizeof(job_t));
    if (!job_q) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(job_worker, "job_worker", 4096, NULL, 4, &job_task, 0);
    if (ok != pdPASS) {
        vQueueDelete(job_q);
        job_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    EL_LOGI(TAG, "Job queue ready");
    return ESP_OK;
}

esp_err_t job_queue_enqueue(const job_t *job)
{
    if (!job_q || !job) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(job_q, job, pdMS_TO_TICKS(10)) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

