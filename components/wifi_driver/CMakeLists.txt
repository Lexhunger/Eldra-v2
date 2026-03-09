idf_component_register(
    SRCS "wifi_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash freertos lwip eldra_logging
)
