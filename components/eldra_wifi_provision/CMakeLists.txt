idf_component_register(
    SRCS "eldra_wifi_provision.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos log esp_wifi esp_netif nvs_flash esp_event eldra_sd eldra_logging
)

set(_prov_write $ENV{ELDRA_WIFI_PROV_WRITE})
if(_prov_write)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ELDRA_WIFI_PROV_WRITE=${_prov_write})
endif()

set(_prov_ssid $ENV{ELDRA_WIFI_SSID})
if(_prov_ssid)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ELDRA_WIFI_PROV_SSID=\"${_prov_ssid}\")
endif()

set(_prov_pass $ENV{ELDRA_WIFI_PASS})
if(_prov_pass)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ELDRA_WIFI_PROV_PASS=\"${_prov_pass}\")
endif()
