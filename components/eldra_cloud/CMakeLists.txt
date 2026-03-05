idf_component_register(
    SRCS "eldra_cloud.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_netif json freertos log esp_timer wifi_driver eldra_logging
)
