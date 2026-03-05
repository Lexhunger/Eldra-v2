#pragma once
#include <stdbool.h>

bool config_store_load_eye_offsets_nvs(int *disp_cx, int *disp_cy, int *eyes_cx, int *eyes_cy);
void config_store_save_eye_offsets_nvs(int disp_cx, int disp_cy, int eyes_cx, int eyes_cy);
void config_store_clear_eye_offsets_nvs(void);
