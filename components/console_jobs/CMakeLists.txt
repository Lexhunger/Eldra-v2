idf_component_register(
    SRCS "console_app.c"
    INCLUDE_DIRS "include"
    REQUIRES console freertos eldra_sensors eldra_logging
)
