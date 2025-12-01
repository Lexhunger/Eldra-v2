/**
 * @file eldra_display_round.c
 * @brief Display driver implementation for Waveshare ESP32-S3 2.8" Round Display
 * 
 * Uses esp_lcd_new_rgb_panel for RGB interface with the round display.
 */

#include "eldra_display_round.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "eldra_display";

/* Waveshare ESP32-S3 2.8" Round Display specifications */
#define DISPLAY_WIDTH      480
#define DISPLAY_HEIGHT     480
#define DISPLAY_HSYNC      40
#define DISPLAY_HBP        40
#define DISPLAY_HFP        48
#define DISPLAY_VSYNC      23
#define DISPLAY_VBP        32
#define DISPLAY_VFP        13
#define DISPLAY_PCLK_HZ    (16 * 1000 * 1000)

/* GPIO pin assignments for Waveshare ESP32-S3 2.8" Round Display */
#define PIN_HSYNC          GPIO_NUM_39
#define PIN_VSYNC          GPIO_NUM_40
#define PIN_DE             GPIO_NUM_41
#define PIN_PCLK           GPIO_NUM_42
#define PIN_BACKLIGHT      GPIO_NUM_2

/* RGB565 data pins (directly mapped to GPIO) */
#define PIN_DATA0          GPIO_NUM_15  /* B0 */
#define PIN_DATA1          GPIO_NUM_7   /* B1 */
#define PIN_DATA2          GPIO_NUM_6   /* B2 */
#define PIN_DATA3          GPIO_NUM_5   /* B3 */
#define PIN_DATA4          GPIO_NUM_4   /* B4 */
#define PIN_DATA5          GPIO_NUM_9   /* G0 */
#define PIN_DATA6          GPIO_NUM_46  /* G1 */
#define PIN_DATA7          GPIO_NUM_3   /* G2 */
#define PIN_DATA8          GPIO_NUM_8   /* G3 */
#define PIN_DATA9          GPIO_NUM_16  /* G4 */
#define PIN_DATA10         GPIO_NUM_1   /* G5 */
#define PIN_DATA11         GPIO_NUM_14  /* R0 */
#define PIN_DATA12         GPIO_NUM_21  /* R1 */
#define PIN_DATA13         GPIO_NUM_47  /* R2 */
#define PIN_DATA14         GPIO_NUM_48  /* R3 */
#define PIN_DATA15         GPIO_NUM_45  /* R4 */

/* Frame buffer size */
#define FB_SIZE            (DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t))

/* Static variables */
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static uint16_t *s_framebuffer = NULL;
static bool s_initialized = false;

esp_err_t eldra_display_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Display already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Waveshare ESP32-S3 2.8\" Round Display");

    /* Configure backlight GPIO */
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_BACKLIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(PIN_BACKLIGHT, 1);

    /* Allocate frame buffer in PSRAM */
    s_framebuffer = heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_framebuffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        return ESP_ERR_NO_MEM;
    }
    memset(s_framebuffer, 0, FB_SIZE);

    ESP_LOGI(TAG, "Frame buffer allocated at %p", s_framebuffer);

    /* Configure RGB panel */
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = DISPLAY_PCLK_HZ,
            .h_res = DISPLAY_WIDTH,
            .v_res = DISPLAY_HEIGHT,
            .hsync_pulse_width = DISPLAY_HSYNC,
            .hsync_back_porch = DISPLAY_HBP,
            .hsync_front_porch = DISPLAY_HFP,
            .vsync_pulse_width = DISPLAY_VSYNC,
            .vsync_back_porch = DISPLAY_VBP,
            .vsync_front_porch = DISPLAY_VFP,
            .flags = {
                .pclk_active_neg = true,
            },
        },
        .data_width = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = 0,
        .psram_trans_align = 64,
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE,
        .pclk_gpio_num = PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            PIN_DATA0, PIN_DATA1, PIN_DATA2, PIN_DATA3,
            PIN_DATA4, PIN_DATA5, PIN_DATA6, PIN_DATA7,
            PIN_DATA8, PIN_DATA9, PIN_DATA10, PIN_DATA11,
            PIN_DATA12, PIN_DATA13, PIN_DATA14, PIN_DATA15,
        },
        .flags = {
            .fb_in_psram = true,
        },
    };

    ESP_LOGI(TAG, "Creating RGB panel");
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &s_panel_handle));

    ESP_LOGI(TAG, "Resetting panel");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));

    ESP_LOGI(TAG, "Initializing panel");
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    s_initialized = true;
    ESP_LOGI(TAG, "Display initialized successfully (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);

    return ESP_OK;
}

esp_err_t eldra_display_clear(uint16_t color)
{
    if (!s_initialized || s_framebuffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
        s_framebuffer[i] = color;
    }

    return eldra_display_refresh();
}

esp_err_t eldra_display_draw_pixel(int x, int y, uint16_t color)
{
    if (!s_initialized || s_framebuffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    s_framebuffer[y * DISPLAY_WIDTH + x] = color;
    return ESP_OK;
}

esp_err_t eldra_display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_initialized || s_framebuffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Clip to display bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH) { w = DISPLAY_WIDTH - x; }
    if (y + h > DISPLAY_HEIGHT) { h = DISPLAY_HEIGHT - y; }

    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            s_framebuffer[row * DISPLAY_WIDTH + col] = color;
        }
    }

    return ESP_OK;
}

int eldra_display_get_width(void)
{
    return DISPLAY_WIDTH;
}

int eldra_display_get_height(void)
{
    return DISPLAY_HEIGHT;
}

uint16_t *eldra_display_get_framebuffer(void)
{
    return s_framebuffer;
}

esp_err_t eldra_display_refresh(void)
{
    if (!s_initialized || s_panel_handle == NULL || s_framebuffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_framebuffer);
}
