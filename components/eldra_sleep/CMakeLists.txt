idf_component_register(
    SRCS "eldra_sleep.c"
    INCLUDE_DIRS "."
    REQUIRES
        eldra_display_round
        eldra_emotion
        eldra_cloud
        eldra_logging
        esp_timer
        esp_system
        esp_wifi
        freertos
        wifi_driver
)
