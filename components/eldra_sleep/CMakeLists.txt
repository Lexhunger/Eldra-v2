idf_component_register(
    SRCS "eldra_sleep.c"
    INCLUDE_DIRS "."
    REQUIRES
        eldra_display_round
        eldra_emotion
        esp_timer
        esp_system
        wifi_driver
)
