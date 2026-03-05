idf_component_register(
    SRCS "rtc_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system esp_timer nvs_flash driver eldra_logging exio
)
