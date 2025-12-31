#pragma once
// Console commands for Wi-Fi (uses wifi_driver, non-blocking via a helper task).

#include "esp_err.h"

esp_err_t ConsoleWiFi_Init(void);
