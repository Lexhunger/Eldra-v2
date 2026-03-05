#include "backlight.h"
#include "eldra_logging.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Hardware binding: LCD backlight gate FET is driven by ESP32-S3 GPIO6.
#define BACKLIGHT_GPIO GPIO_NUM_6

#define BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_RESOLUTION LEDC_TIMER_10_BIT
#define BACKLIGHT_LEDC_MAX_RAW ((1U << 10) - 1)

static const char *TAG = "backlight";
static bool s_pwm_ready = false;
static uint16_t s_cached_raw = 0;

static uint16_t percent_to_raw(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    uint32_t raw = (uint32_t)CONFIG_BACKLIGHT_MAX_RAW_AT_FULL_PERCENT * percent / 100U;
    if (raw > BACKLIGHT_LEDC_MAX_RAW) {
        raw = BACKLIGHT_LEDC_MAX_RAW;
    }
    return (uint16_t)raw;
}

void backlight_init_off(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BACKLIGHT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(BACKLIGHT_GPIO, 0);
    s_cached_raw = 0;
    s_pwm_ready = false; // LEDC setup is deferred until after LCD init.
}

void backlight_setup_pwm(void) {
    EL_LOGI(TAG, "Configuring PWM: freq=%d Hz, resolution=%d-bit", CONFIG_BACKLIGHT_PWM_FREQ_HZ, BACKLIGHT_LEDC_RESOLUTION);
    ledc_timer_config_t ledc_timer = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .duty_resolution = BACKLIGHT_LEDC_RESOLUTION,
        .freq_hz = CONFIG_BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        EL_LOGE(TAG, "LEDC timer config failed: %d", err);
        return;
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BACKLIGHT_GPIO,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        EL_LOGE(TAG, "LEDC channel config failed: %d", err);
        return;
    }

    s_pwm_ready = true;
    backlight_set_raw(s_cached_raw);
    EL_LOGI(TAG, "PWM ready on GPIO%d (raw duty=%u)", BACKLIGHT_GPIO, s_cached_raw);
}

void backlight_set_raw(uint16_t duty) {
    if (duty > BACKLIGHT_LEDC_MAX_RAW) {
        duty = BACKLIGHT_LEDC_MAX_RAW;
    }
    s_cached_raw = duty;
    if (!s_pwm_ready) {
        // Cache the request; caller should run backlight_setup_pwm later.
        EL_LOGW(TAG, "set_raw before PWM init; caching duty=%u", duty);
        return;
    }
    if (ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty) != ESP_OK) {
        EL_LOGE(TAG, "Failed to set LEDC duty");
        return;
    }
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
}

void backlight_set_brightness_percent(uint8_t percent) {
    backlight_set_raw(percent_to_raw(percent));
}

void backlight_ramp_to_percent(uint8_t target_percent, uint16_t step_ms) {
    if (!CONFIG_BACKLIGHT_ENABLE_RAMP) {
        backlight_set_brightness_percent(target_percent);
        return;
    }

    if (step_ms == 0) {
        step_ms = 1;
    }

    const uint16_t target_raw = percent_to_raw(target_percent);
    int32_t current = s_cached_raw;
    int32_t step_raw = (int32_t)percent_to_raw(CONFIG_BACKLIGHT_RAMP_STEP_PERCENT);
    if (step_raw < 1) {
        step_raw = 1;
    }
    if (!s_pwm_ready) {
        EL_LOGW(TAG, "ramp requested before PWM init; applying target directly");
        backlight_set_raw(target_raw);
        return;
    }

    while (current != target_raw) {
        if (target_raw > current) {
            current += step_raw;
            if (current > target_raw) {
                current = target_raw;
            }
        } else {
            current -= step_raw;
            if (current < target_raw) {
                current = target_raw;
            }
        }
        backlight_set_raw((uint16_t)current);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
}

uint16_t backlight_get_raw(void) {
    return s_cached_raw;
}

