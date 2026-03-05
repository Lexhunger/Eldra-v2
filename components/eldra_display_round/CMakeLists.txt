idf_component_register(
    SRCS
        "eldra_display_round.c"
        "eldra_glyphs.c"
        "ST7701S.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        assets/glyphs/sleep/glyph_sleep.png
    REQUIRES
        backlight
        esp_lcd
        esp_mm
        driver
        freertos
        esp_timer
        exio
        eldra_logging
)
