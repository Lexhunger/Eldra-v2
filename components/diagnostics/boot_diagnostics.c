#include "boot_diagnostics.h"

#include <stdbool.h>
#include <stddef.h>

#include "Buzzer.h"
#include "backlight.h"
#include "eldra_display_round.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "boot_diag";

static void buzzer_pulse(uint32_t duration_ms) {
    Buzzer_On();
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    Buzzer_Off();
}

static void buzzer_sos_pattern(void) {
    // SOS in Morse: dit dit dit, dah dah dah, dit dit dit.
    const uint32_t dit = 150;
    const uint32_t dah = dit * 3;
    const uint32_t intra_gap = dit;
    const uint32_t letter_gap = dit * 3;
    const uint32_t word_gap = dit * 7;

    for (int i = 0; i < 3; ++i) {
        buzzer_pulse(dit);
        vTaskDelay(pdMS_TO_TICKS(intra_gap));
    }

    vTaskDelay(pdMS_TO_TICKS(letter_gap));

    for (int i = 0; i < 3; ++i) {
        buzzer_pulse(dah);
        vTaskDelay(pdMS_TO_TICKS(intra_gap));
    }

    vTaskDelay(pdMS_TO_TICKS(letter_gap));

    for (int i = 0; i < 3; ++i) {
        buzzer_pulse(dit);
        vTaskDelay(pdMS_TO_TICKS(intra_gap));
    }

    vTaskDelay(pdMS_TO_TICKS(word_gap));
}

static void draw_error_frame(uint16_t *framebuffer, int width, int height, bool invert) {
    if (!framebuffer || width <= 0 || height <= 0) {
        return;
    }
    const uint16_t color = invert ? 0x0000 : 0xF800; // alternate black/red
    const size_t pixels = (size_t)width * (size_t)height;
    for (size_t i = 0; i < pixels; ++i) {
        framebuffer[i] = color;
    }
    (void)eldra_display_round_blit(framebuffer, width, height);
}

void diag_init(void) {
    // Ensure the buzzer is off before any diagnostics patterns run.
    Buzzer_Off();
}

void diag_signal_error_lcd_init(uint16_t *framebuffer, int width, int height) {
    ESP_LOGE(TAG, "LCD init/test failed; entering diagnostic SOS loop");
    backlight_set_brightness_percent(CONFIG_BACKLIGHT_DIAG_PERCENT);

    bool invert = false;
    while (1) {
        draw_error_frame(framebuffer, width, height, invert);
        invert = !invert;
        buzzer_sos_pattern();
    }
}
