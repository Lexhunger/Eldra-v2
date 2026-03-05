idf_component_register(
    SRCS "runtime_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        esp_timer
        esp_system
        heap
        eldra_comms
        eldra_display_round
        sd_driver
        eldra_logging
)
