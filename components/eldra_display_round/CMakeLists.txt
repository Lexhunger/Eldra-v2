idf_component_register(
    SRCS
        "eldra_display_round.c"
        "ST7701S.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        esp_wifi
        esp_netif
        eldra_sensors
        esp_lcd
        driver
        esp_timer
        freertos
)
