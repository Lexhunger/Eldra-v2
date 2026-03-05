#include "mood_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_err.h"

static const char *NS = "eldra_mood";

static esp_err_t ensure_nvs(void)
{
    static bool init_done = false;
    if (init_done) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) init_done = true;
    return err;
}

bool config_store_load_mood_nvs(int *hap, int *hun, int *eng, int *soc, int *fear, int *eld, int *state)
{
    if (ensure_nvs() != ESP_OK) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t a= -1,b= -1,c= -1,d= -1,e= -1,f= -1,g= -1;
    esp_err_t er =
        nvs_get_i32(h, "hap", &a) |
        nvs_get_i32(h, "hun", &b) |
        nvs_get_i32(h, "eng", &c) |
        nvs_get_i32(h, "soc", &d) |
        nvs_get_i32(h, "fear", &e)|
        nvs_get_i32(h, "eld", &f) |
        nvs_get_i32(h, "st",  &g);
    nvs_close(h);
    if (er == ESP_OK) {
        if (hap) *hap = (int)a;
        if (hun) *hun = (int)b;
        if (eng) *eng = (int)c;
        if (soc) *soc = (int)d;
        if (fear) *fear = (int)e;
        if (eld) *eld = (int)f;
        if (state) *state = (int)g;
        return true;
    }
    return false;
}

void config_store_save_mood_nvs(int hap, int hun, int eng, int soc, int fear, int eld, int state)
{
    if (ensure_nvs() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "hap", hap);
    nvs_set_i32(h, "hun", hun);
    nvs_set_i32(h, "eng", eng);
    nvs_set_i32(h, "soc", soc);
    nvs_set_i32(h, "fear", fear);
    nvs_set_i32(h, "eld", eld);
    nvs_set_i32(h, "st",  state);
    nvs_commit(h);
    nvs_close(h);
}

void config_store_clear_mood_nvs(void)
{
    if (ensure_nvs() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_erase_all(h);
    (void)nvs_commit(h);
    nvs_close(h);
}
