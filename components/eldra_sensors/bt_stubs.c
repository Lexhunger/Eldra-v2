#include "esp_err.h"
#include "esp_gap_ble_api.h"

// Weak fallbacks in case the Bluedroid BLE GAP symbols are not linked in (build-time safety only).
__attribute__((weak)) esp_err_t esp_ble_gap_set_scan_params(esp_ble_scan_params_t *scan_params) {
    (void)scan_params;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t esp_ble_gap_start_scanning(uint32_t duration) {
    (void)duration;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t esp_ble_gap_stop_scanning(void) {
    return ESP_ERR_NOT_SUPPORTED;
}
