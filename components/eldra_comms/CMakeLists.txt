idf_component_register(
    SRCS "eldra_comms.c" "console.c" "command_router.c"
    INCLUDE_DIRS "."
    REQUIRES eldra_emotion eldra_logging eldra_sensors driver
)
