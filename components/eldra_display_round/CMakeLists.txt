idf_component_register(
    SRCS
        "eldra_display_round.c"
        "ST7701S.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        driver
        freertos
        esp_timer
        exio
)
