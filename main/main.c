#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @file main.c
 * @brief Application entry point for Eldra-V2.
 */

#include "eldra_display_round.h"
#include "eldra_emotion.h"
#include "eldra_comms.h"
#include "eldra_logging.h"
#include "eldra_eyes.h"
#include "eldra_sensors.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

static const char *TAG = "app_main";
static eldra_eyes_context_t *g_eyes_ctx = NULL;

#if defined(ELDRA_DEBUG_SET_RTC)
// Edit these values or define ELDRA_DEBUG_SET_RTC to set RTC once at boot for validation.
static const datetime_t k_debug_rtc_time = {
    .year = 2025,
    .month = 1,
    .day = 1,
    .dotw = 3, // Wednesday
    .hour = 12,
    .minute = 0,
    .second = 0,
};
#endif

static void imu_callback(float gx_dps, float gy_dps, float gz_dps,
                         float ax_g, float ay_g, float az_g, uint32_t dt_ms) {
    if (g_eyes_ctx) {
        eldra_eyes_handle_imu(g_eyes_ctx, gx_dps, gy_dps, gz_dps, ax_g, ay_g, az_g, dt_ms);
    }
}

/**
 * @brief Convert epoch milliseconds to datetime_t (UTC).
 */
/**
 * @brief Sketch of the future command/emotion plumbing without hardware drivers.
 *        Compile-time guard prevents it from running unless explicitly enabled.
 */
static void run_emotion_backbone_demo(void) __attribute__((unused));
static void run_emotion_backbone_demo(void) {
    emotion_context_t emotion = {0};
    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    log_init();
    if (eldra_sensors_init() != ESP_OK) {
        ESP_LOGE(TAG, "Sensor init failed; cannot run emotion demo");
        return;
    }
    emotion_init(&emotion, start_ms);
    comms_init();
    comms_commands_init(&emotion);

    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        emotion_set_battery_percent(&emotion, eldra_sensors_get_battery_percent());
        emotion_on_tick(&emotion, now_ms);
        comms_process_all_pending(&emotion, now_ms);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
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
#if defined(ELDRA_EMOTION_BACKBONE_DEMO)
    run_emotion_backbone_demo();
    return;
#endif

    log_init();

    if (eldra_sensors_init() != ESP_OK) {
        ESP_LOGE(TAG, "Sensor init failed; holding");
        goto fail_safe;
    }
    ESP_LOGI(TAG, "Sensors init complete");

    // Initialize command router (shared by UART console and future HTTP) with no emotion context yet.
    comms_commands_init(NULL);
    // Start interactive console for commands/log retrieval.
    comms_console_start();

#if defined(ELDRA_DEBUG_SET_RTC)
    // One-shot RTC set for backup-battery validation.
    if (eldra_sensors_rtc_set(&k_debug_rtc_time) != ESP_OK) {
        ESP_LOGW(TAG, "RTC set failed");
    } else {
        datetime_t now = {0};
        eldra_sensors_rtc_get(&now);
        char ts[64] = {0};
        datetime_to_str(ts, now);
        ESP_LOGI(TAG, "RTC now %s (debug set enabled)", ts);
    }
#endif

    if (eldra_display_round_init() != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed; holding");
        goto fail_safe;
    }
    ESP_LOGI(TAG, "Display init complete");

    // Ensure the backlight is on (vendor default may be 0).
    eldra_display_round_set_backlight(90);

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
    vTaskDelay(pdMS_TO_TICKS(200));

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
