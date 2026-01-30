idf_component_register(
    SRCS "console_sd.c"
    INCLUDE_DIRS "include"
    REQUIRES console sd_driver freertos config_store wifi_driver eldra_cloud eldra_sleep eldra_emotion
)
