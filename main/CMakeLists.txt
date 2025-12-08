idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES eldra_display_round eldra_sensors eldra_eyes eldra_emotion eldra_comms eldra_logging
)
