idf_component_register(
    SRCS "boot_diagnostics.c"
    INCLUDE_DIRS "."
    REQUIRES
        backlight
        eldra_display_round
        eldra_sensors
        eldra_logging
        freertos
)
