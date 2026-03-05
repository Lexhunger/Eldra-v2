#pragma once
#include <stdbool.h>

bool config_store_load_mood_nvs(int *hap, int *hun, int *eng, int *soc, int *fear, int *eld, int *state);
void config_store_save_mood_nvs(int hap, int hun, int eng, int soc, int fear, int eld, int state);
void config_store_clear_mood_nvs(void);
