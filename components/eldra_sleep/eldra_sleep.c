#include "eldra_sleep.h"
#include "eldra_glyphs.h"
#include "eldra_emotion.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "eldra_sleep";

static bool s_sleeping = false;
static bool s_shutdown_scheduled = false;
static uint64_t s_shutdown_time_ms = 0;
static bool s_glyph_inited = false;
static emotion_context_t *s_emotion = NULL;

void eldra_sleep_init(emotion_context_t *ctx)
{
    s_emotion = ctx;
    eldra_glyphs_init(NULL);
    s_glyph_inited = true;
    ESP_LOGI(TAG, "sleep subsystem ready");
}

void eldra_sleep_sleep_now(void)
{
    uint32_t delay_ms = 120000 + (esp_random() % 180000); // 2–5 minutes
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    s_shutdown_time_ms = now_ms + delay_ms;
    s_sleeping = true;
    s_shutdown_scheduled = false;
    if (s_glyph_inited) {
        eldra_glyphs_show(GLYPH_SLEEP);
    }
    if (s_emotion) {
        emotion_force_sleep(s_emotion, true, now_ms);
    }
    ESP_LOGI(TAG, "Sleep requested; shutdown staging in %u ms", delay_ms);
}

void eldra_sleep_wake_now(void)
{
    s_sleeping = false;
    s_shutdown_scheduled = false;
    s_shutdown_time_ms = 0;
    if (s_glyph_inited) {
        eldra_glyphs_hide();
    }
    if (s_emotion) {
        emotion_force_sleep(s_emotion, false, esp_timer_get_time() / 1000ULL);
    }
    ESP_LOGI(TAG, "Wake requested; sleep cancelled");
}

void eldra_sleep_set_glyph_offset(int dx, int dy)
{
    if (s_glyph_inited) {
        eldra_glyphs_set_offset(dx, dy);
    }
}

void eldra_sleep_tick(uint64_t now_ms)
{
    if (!s_sleeping) return;
    if (!s_shutdown_scheduled && s_shutdown_time_ms > 0 && now_ms >= s_shutdown_time_ms) {
        s_shutdown_scheduled = true;
        ESP_LOGW(TAG, "Sleep window reached; would stage power-down here");
        // TODO: add real shutdown hooks (wifi off, backlight dim, etc.)
    }
}
