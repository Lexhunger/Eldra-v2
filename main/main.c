#include <stdio.h>

/**
 * @file main.c
 * @brief Application entry point for Eldra-V2.
 */

#include "eldra_display_round.h"
#include "eldra_eyes.h"
#include "eldra_sensors.h"
#include "driver/gpio.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_main";
static eldra_eyes_context_t *g_eyes_ctx = NULL;

static void imu_callback(float gx_dps, float gy_dps, float gz_dps,
                         float ax_g, float ay_g, float az_g, uint32_t dt_ms) {
    if (g_eyes_ctx) {
        eldra_eyes_handle_imu(g_eyes_ctx, gx_dps, gy_dps, gz_dps, ax_g, ay_g, az_g, dt_ms);
    }
}

static void force_backlight_gpio_low(void) {
    // Ensure the backlight FET is off before any init to reduce inrush/current spikes.
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_6,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(GPIO_NUM_6, 0);
}

/**
 * @brief Application entry point.
 *
 * Boot order:
 * 1) Initialize sensors/peripherals (I2C, IMU, RTC, SD, etc.).
 * 2) Initialize the round RGB display.
 * 3) Render the static chibi eyes into a raw framebuffer and push to the panel.
 */
void app_main(void) {
    force_backlight_gpio_low();

    if (eldra_sensors_init() != ESP_OK) {
        ESP_LOGE(TAG, "Sensor init failed; holding");
        goto fail_safe;
    }
    ESP_LOGI(TAG, "Sensors init complete");

    // Small settle to let rails stabilize before the LCD pulls current.
    vTaskDelay(pdMS_TO_TICKS(200));

    if (eldra_display_round_init() != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed; holding");
        goto fail_safe;
    }
    ESP_LOGI(TAG, "Display init complete");
    // Ensure backlight stays off until we have drawn a known frame.
    eldra_display_round_set_backlight(0);

    const int fb_width = eldra_display_round_get_width();
    const int fb_height = eldra_display_round_get_height();

    eldra_eyes_context_t *eyes_ctx = eldra_eyes_create();
    if (!eyes_ctx) {
        ESP_LOGE(TAG, "Failed to create eyes context; holding");
        goto fail_safe;
    }
    g_eyes_ctx = eyes_ctx;
    ESP_LOGI(TAG, "Eyes context created");
    eldra_sensors_set_imu_callback(imu_callback);
    ESP_LOGI(TAG, "IMU callback registered");

    size_t buf_size_bytes = (size_t)fb_width * (size_t)fb_height * sizeof(uint16_t);
    uint16_t *framebuffer = (uint16_t *)heap_caps_malloc(buf_size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!framebuffer) {
        framebuffer = (uint16_t *)heap_caps_malloc(buf_size_bytes, MALLOC_CAP_8BIT);
        if (!framebuffer) {
            ESP_LOGE(TAG, "Framebuffer alloc failed; holding");
            goto fail_safe;
        }
    }

    // Sanity: draw a solid test frame once so we know the panel path works.
    for (size_t i = 0; i < (size_t)fb_width * (size_t)fb_height; ++i) {
        framebuffer[i] = 0xFFFF; // white
    }
    esp_err_t test_blit = eldra_display_round_blit(framebuffer, fb_width, fb_height);
    ESP_LOGI(TAG, "Test pattern blit result=%d", test_blit);
    if (test_blit != ESP_OK) {
        ESP_LOGW(TAG, "Re-resetting panel and retrying blit due to init-time glitch");
        if (eldra_display_round_reset_panel() == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            test_blit = eldra_display_round_blit(framebuffer, fb_width, fb_height);
            ESP_LOGI(TAG, "Retry blit result=%d", test_blit);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    // Backlight was forced low during boot; bring it up gently after a known frame.
    const uint8_t target_bl = 80;
    for (uint8_t lvl = 0; lvl <= target_bl; lvl += 5) {
        eldra_display_round_set_backlight(lvl);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Simple real-time loop (~60 FPS): update eyes, render, and push to panel.
    uint64_t last_us = esp_timer_get_time();

    while (1) {
        uint64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - last_us) / 1000ULL);
        if (dt_ms == 0) {
            dt_ms = 1;
        }
        last_us = now_us;

        eldra_eyes_update(eyes_ctx, dt_ms);
        eldra_eyes_render(eyes_ctx, framebuffer, (uint16_t)fb_width, (uint16_t)fb_height);
        esp_err_t blit_ret = eldra_display_round_blit(framebuffer, fb_width, fb_height);
        if (blit_ret != ESP_OK) {
            ESP_LOGE(TAG, "Blit failed: %d", blit_ret);
        }

        vTaskDelay(pdMS_TO_TICKS(16)); // ~60 FPS pacing
    }

fail_safe:
    // Fail-safe steady state: sleep indefinitely to prevent watchdog spam/log floods.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
