idf_component_register(
    SRCS
        "eldra_sensors.c"
        "BAT_Driver.c"
        "Button_Driver.c"
        "multi_button.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        esp_timer
        esp_adc
        eldra_logging
        imu_qmi8658
        exio
        rtc_driver
)
