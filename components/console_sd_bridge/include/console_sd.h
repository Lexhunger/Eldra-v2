#pragma once

#include "esp_err.h"

// Register SD console commands (sd_init, sd_list, sd_read, sd_delete, sd_log, sd_wifi_save)
esp_err_t ConsoleSD_Init(void);

