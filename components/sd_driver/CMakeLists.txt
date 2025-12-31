idf_component_register(
    SRCS "sd_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio freertos exio esp_timer
)
