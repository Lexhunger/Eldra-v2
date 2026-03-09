idf_component_register(
    SRCS "config_store.c" "eye_offsets_nvs.c" "mood_nvs.c"
    INCLUDE_DIRS "include"
    REQUIRES json sd_driver nvs_flash eldra_logging
)
