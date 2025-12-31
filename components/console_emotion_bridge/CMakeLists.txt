idf_component_register(
    SRCS "console_emotion.c"
    INCLUDE_DIRS "include"
    REQUIRES console eldra_comms eldra_emotion eldra_eyes config_store freertos
)
