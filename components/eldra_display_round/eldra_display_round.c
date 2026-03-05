#include "eldra_display_round.h"
#include "ST7701S.h"
#include "eldra_logging.h"
#include "esp_lcd_panel_rgb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "eldra_display_round";
static const TickType_t k_panel_mutex_wait_blit_ticks = pdMS_TO_TICKS(2);
static const TickType_t k_panel_mutex_wait_ctrl_ticks = pdMS_TO_TICKS(150);

static bool s_panel_ready = false;
static bool s_reinit_in_progress = false;
static bool s_panel_sleeping = false;

static SemaphoreHandle_t s_panel_mutex = NULL;
static uint32_t s_panel_ready_stamp_ms = 0;
static uint32_t s_panel_failures = 0;
static bool s_logged_ready = false;
static uint8_t *s_panel_fbs[2] = {0};
static uint8_t s_panel_fb_count = 0;
static size_t s_panel_fb_bytes = 0;
static uint32_t s_reset_count = 0;
static uint32_t s_sync_count = 0;
static uint32_t s_sleep_enter_count = 0;
static uint32_t s_sleep_exit_count = 0;
static uint32_t s_blit_ok_count = 0;
static uint32_t s_blit_fail_count = 0;
static uint32_t s_blit_timeout_count = 0;
static uint32_t s_last_blit_ok_ms = 0;
static uint32_t s_last_blit_fail_ms = 0;
static esp_err_t s_last_blit_err = ESP_OK;

static inline bool panel_lock_take_timeout(TickType_t wait_ticks)
{
    if (!s_panel_mutex) {
        return false;
    }
    return (xSemaphoreTake(s_panel_mutex, wait_ticks) == pdTRUE);
}

static esp_err_t panel_normalize_locked(void)
{
    return ST7701S_apply_runtime_panel_defaults();
}

static void panel_cache_frame_buffers_locked(void)
{
    s_panel_fbs[0] = NULL;
    s_panel_fbs[1] = NULL;
    s_panel_fb_count = 0;
    s_panel_fb_bytes = (size_t)EXAMPLE_LCD_H_RES * (size_t)EXAMPLE_LCD_V_RES * sizeof(uint16_t);
    if (!panel_handle) {
        return;
    }

    // Query a single panel-owned framebuffer only. Requesting "2" first on
    // one-buffer panels emits an IDF error log even though fallback succeeds.
    void *fb0 = NULL;
    esp_err_t err = esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 1, &fb0);
    if (err == ESP_OK && fb0) {
        s_panel_fbs[0] = (uint8_t *)fb0;
        s_panel_fb_count = 1;
    }

    if (s_panel_fb_count > 0) {
        for (uint8_t i = 0; i < s_panel_fb_count; ++i) {
            memset(s_panel_fbs[i], 0, s_panel_fb_bytes);
        }
        EL_LOGI(TAG, "Panel framebuffer mode: %u internal buffer(s) (submit path uses panel framebuffer with cache sync)",
                (unsigned)s_panel_fb_count);
    } else {
        EL_LOGW(TAG, "Panel framebuffer query unavailable; using external draw buffer path");
    }
}

bool eldra_display_round_is_ready(void)
{
    return s_panel_ready;
}

void eldra_display_round_get_diag(eldra_display_round_diag_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->panel_ready = s_panel_ready;
    out->panel_sleeping = s_panel_sleeping;
    out->reinit_in_progress = s_reinit_in_progress;
    out->panel_ready_stamp_ms = s_panel_ready_stamp_ms;
    out->reset_count = s_reset_count;
    out->sync_count = s_sync_count;
    out->sleep_enter_count = s_sleep_enter_count;
    out->sleep_exit_count = s_sleep_exit_count;
    out->blit_ok_count = s_blit_ok_count;
    out->blit_fail_count = s_blit_fail_count;
    out->blit_timeout_count = s_blit_timeout_count;
    out->last_blit_ok_ms = s_last_blit_ok_ms;
    out->last_blit_fail_ms = s_last_blit_fail_ms;
    out->last_blit_err = s_last_blit_err;
}

// Simple color bars for debugging; draws vertical bands.
esp_err_t eldra_display_round_testpattern(void)
{
    if (!panel_handle) return ESP_ERR_INVALID_STATE;
    const uint16_t colors[] = {
        0xF800, // red
        0x07E0, // green
        0x001F, // blue
        0xFFE0, // yellow
        0xFFFF  // white
    };
    int bands = sizeof(colors) / sizeof(colors[0]);
    int band_w = EXAMPLE_LCD_H_RES / bands;
    const int chunk_lines = 24;
    size_t chunk_pixels = (size_t)EXAMPLE_LCD_H_RES * (size_t)chunk_lines;
    uint16_t *line = heap_caps_malloc(chunk_pixels * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!line) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    for (int y = 0; y < EXAMPLE_LCD_V_RES && err == ESP_OK; y += chunk_lines) {
        int lines = chunk_lines;
        if (y + lines > EXAMPLE_LCD_V_RES) lines = EXAMPLE_LCD_V_RES - y;
        for (int row = 0; row < lines; ++row) {
            uint16_t *p = line + row * EXAMPLE_LCD_H_RES;
            for (int x = 0; x < EXAMPLE_LCD_H_RES; ++x) {
                int band = x / band_w;
                if (band >= bands) band = bands - 1;
                p[x] = colors[band];
            }
        }
        err = esp_lcd_panel_draw_bitmap(panel_handle, 0, y, EXAMPLE_LCD_H_RES, y + lines, line);
    }
    heap_caps_free(line);
    return err;
}

esp_err_t eldra_display_round_init(void)
{
    if (s_panel_mutex == NULL) {
        s_panel_mutex = xSemaphoreCreateMutex();
    }
    return eldra_display_round_reset_panel();
}

void eldra_display_round_set_backlight(uint8_t level)
{
    Set_Backlight(level);
}

esp_err_t eldra_display_round_reset_panel(void)
{
    if (!s_panel_mutex) {
        s_panel_mutex = xSemaphoreCreateMutex();
    }
    if (!s_panel_mutex) {
        return ESP_ERR_NO_MEM;
    }

    if (!panel_lock_take_timeout(k_panel_mutex_wait_ctrl_ticks)) {
        return ESP_ERR_TIMEOUT;
    }
    const char *caller = pcTaskGetName(NULL);
    EL_LOGI(TAG, "Panel reset requested by task=%s", caller ? caller : "?");

    s_reinit_in_progress = true;
    s_panel_ready = false;
    s_panel_sleeping = false;
    s_logged_ready = false;
    s_panel_failures = 0;
    s_panel_fbs[0] = NULL;
    s_panel_fbs[1] = NULL;
    s_panel_fb_count = 0;
    s_panel_fb_bytes = 0;

    // Brief backlight dip only when LEDC/panel already existed.
    if (panel_handle != NULL) {
        Set_Backlight(0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    esp_err_t err = LCD_Init();
    if (err != ESP_OK) {
        EL_LOGE(TAG, "LCD_Init failed during reset: %s", esp_err_to_name(err));
        s_reinit_in_progress = false;
        xSemaphoreGive(s_panel_mutex);
        return err;
    }

    // Demo-aligned path: render into panel-owned frame buffers and present via draw_bitmap.
    panel_cache_frame_buffers_locked();

    // Give RGB timings/panel a short settle window before accepting blits.
    vTaskDelay(pdMS_TO_TICKS(80));
    Set_Backlight(90);

    s_panel_ready = true;
    s_reinit_in_progress = false;
    s_reset_count++;
    xSemaphoreGive(s_panel_mutex);
    EL_LOGI(TAG, "Panel reset complete (task=%s reset_count=%u)", caller ? caller : "?", s_reset_count);
    return ESP_OK;
}

esp_err_t eldra_display_round_sync_panel_state(void)
{
    if (!panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_panel_mutex) {
        s_panel_mutex = xSemaphoreCreateMutex();
    }
    if (!s_panel_mutex) {
        return ESP_ERR_NO_MEM;
    }

    if (!panel_lock_take_timeout(k_panel_mutex_wait_ctrl_ticks)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = panel_normalize_locked();
    xSemaphoreGive(s_panel_mutex);
    if (err == ESP_OK) {
        s_sync_count++;
        const char *caller = pcTaskGetName(NULL);
        EL_LOGI(TAG, "Panel runtime state synced (task=%s sync_count=%u)", caller ? caller : "?", s_sync_count);
    }
    return err;
}

int eldra_display_round_get_width(void)
{
    return EXAMPLE_LCD_H_RES;
}

int eldra_display_round_get_height(void)
{
    return EXAMPLE_LCD_V_RES;
}

void eldra_display_round_set_shift(int dx, int dy)
{
    (void)dx;
    (void)dy;
    EL_LOGW(TAG, "display_shift is disabled; use renderer offsets (disp_center/eyes_offset)");
}

esp_err_t eldra_display_round_blit(const uint16_t *framebuffer, int width, int height)
{
    if (!s_panel_ready || s_panel_sleeping || s_reinit_in_progress) {
        // Skip quietly if panel not ready; caller can retry on next frame.
        s_panel_failures++;
        return ESP_ERR_INVALID_STATE;
    }
    if (!panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!framebuffer || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (width != EXAMPLE_LCD_H_RES || height != EXAMPLE_LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_panel_mutex) {
        if (!panel_lock_take_timeout(k_panel_mutex_wait_blit_ticks)) {
            s_panel_failures++;
            s_blit_timeout_count++;
            s_blit_fail_count++;
            s_last_blit_err = ESP_ERR_TIMEOUT;
            s_last_blit_fail_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            return ESP_ERR_TIMEOUT;
        }
    }
    esp_err_t err = ESP_OK;
    size_t frame_bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
    if (s_panel_fb_count > 0 &&
        s_panel_fbs[0] != NULL &&
        s_panel_fb_bytes >= frame_bytes) {
        // Match the demo/runtime model more closely: keep the panel's own scan
        // buffer hot instead of depending on draw_bitmap to push a transient
        // frame after reset/wake.
        memcpy(s_panel_fbs[0], framebuffer, frame_bytes);
        err = esp_cache_msync(s_panel_fbs[0],
                              frame_bytes,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (err != ESP_OK) {
            // Fall back to the driver path if cache sync for the panel-owned
            // buffer fails. This preserves a visible frame even if the direct
            // buffer path is unavailable on this target/runtime.
            err = esp_lcd_panel_draw_bitmap(panel_handle,
                                            0,
                                            0,
                                            width,
                                            height,
                                            framebuffer);
        }
    } else {
        // Fallback when the RGB driver does not expose a persistent framebuffer.
        err = esp_lcd_panel_draw_bitmap(panel_handle,
                                        0,
                                        0,
                                        width,
                                        height,
                                        framebuffer);
    }
    if (s_panel_mutex) {
        xSemaphoreGive(s_panel_mutex);
    }

    if (err == ESP_OK && !s_logged_ready) {
        s_logged_ready = true;
        s_panel_ready_stamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        EL_LOGI(TAG, "Panel first successful blit (ready, failures=%u)", s_panel_failures);
        s_panel_failures = 0;
    }
    if (err == ESP_OK) {
        s_blit_ok_count++;
        s_last_blit_ok_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    } else {
        s_blit_fail_count++;
        s_last_blit_err = err;
        s_last_blit_fail_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_TIMEOUT) {
        EL_LOGE(TAG, "blit failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t eldra_display_round_panel_sleep(bool enable)
{
    if (!panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_panel_mutex) {
        s_panel_mutex = xSemaphoreCreateMutex();
    }
    if (!s_panel_mutex) {
        return ESP_ERR_NO_MEM;
    }

    if (!panel_lock_take_timeout(k_panel_mutex_wait_ctrl_ticks)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;

    if (enable) {
        if (s_panel_sleeping) {
            xSemaphoreGive(s_panel_mutex);
            return ESP_OK;
        }
        err = ST7701S_display_off();
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            err = ST7701S_sleep_in();
            vTaskDelay(pdMS_TO_TICKS(130));
        }
        if (err == ESP_OK) {
            s_panel_sleeping = true;
            s_panel_ready = false;
            s_sleep_enter_count++;
            EL_LOGI(TAG, "Panel entered sleep");
        }
    } else {
        if (!s_panel_sleeping) {
            xSemaphoreGive(s_panel_mutex);
            return ESP_OK;
        }
        // Use full reset path on wake for deterministic scan origin/state.
        s_panel_sleeping = false;
        s_panel_ready = false;
        s_logged_ready = false;
        xSemaphoreGive(s_panel_mutex);
        err = eldra_display_round_reset_panel();
        if (err == ESP_OK) {
            s_sleep_exit_count++;
            EL_LOGI(TAG, "Panel exited sleep (full reset)");
        }
        return err;
    }

    xSemaphoreGive(s_panel_mutex);
    return err;
}
