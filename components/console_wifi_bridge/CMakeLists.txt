idf_component_register(
    SRCS "console_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES console wifi_driver freertos
)
