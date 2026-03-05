#include "eye_offsets_nvs.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "eye_offsets_nvs";
static const char *NS = "eldra_offsets";

static esp_err_t ensure_nvs(void)
{
    static bool init_done = false;
    if (init_done) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        init_done = true;
    }
    return err;
}

bool config_store_load_eye_offsets_nvs(int *disp_cx, int *disp_cy, int *eyes_cx, int *eyes_cy)
{
    if (ensure_nvs() != ESP_OK) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t dx=0, dy=0, ex=0, ey=0;
    esp_err_t er1 = nvs_get_i32(h, "disp_cx", &dx);
    esp_err_t er2 = nvs_get_i32(h, "disp_cy", &dy);
    esp_err_t er3 = nvs_get_i32(h, "eyes_cx", &ex);
    esp_err_t er4 = nvs_get_i32(h, "eyes_cy", &ey);
    nvs_close(h);
    if (er1==ESP_OK && er2==ESP_OK && er3==ESP_OK && er4==ESP_OK) {
        if (disp_cx) *disp_cx = (int)dx;
        if (disp_cy) *disp_cy = (int)dy;
        if (eyes_cx) *eyes_cx = (int)ex;
        if (eyes_cy) *eyes_cy = (int)ey;
        return true;
    }
    return false;
}

void config_store_save_eye_offsets_nvs(int disp_cx, int disp_cy, int eyes_cx, int eyes_cy)
{
    if (ensure_nvs() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "disp_cx", disp_cx);
    nvs_set_i32(h, "disp_cy", disp_cy);
    nvs_set_i32(h, "eyes_cx", eyes_cx);
    nvs_set_i32(h, "eyes_cy", eyes_cy);
    nvs_commit(h);
    nvs_close(h);
}

void config_store_clear_eye_offsets_nvs(void)
{
    if (ensure_nvs() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_erase_all(h);
    (void)nvs_commit(h);
    nvs_close(h);
}
