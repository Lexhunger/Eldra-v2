/**
 * @file main.c
 * @brief Main application for Eldra-v2 on Waveshare ESP32-S3 2.8" Round Display
 * 
 * Initializes NVS and display, then draws a centered eye.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "eldra_display_round.h"
#include "eldra_eyes.h"

static const char *TAG = "eldra_main";

/* Eye scale factor - adjusts size on 480x480 display */
#define EYE_SCALE 20

void app_main(void)
{
    ESP_LOGI(TAG, "Eldra-v2 starting...");

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* Initialize display */
    ret = eldra_display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Display initialized");

    /* Clear display to black */
    ret = eldra_display_clear(0x0000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear display: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Display cleared");

    /* Initialize eyes module */
    ret = eldra_eyes_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize eyes: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Eyes module initialized");

    /* Draw centered eye */
    ret = eldra_eyes_draw_centered(EYE_SCALE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to draw eye: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Eye drawn successfully");

    ESP_LOGI(TAG, "Eldra-v2 initialization complete");

    /* Main loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
