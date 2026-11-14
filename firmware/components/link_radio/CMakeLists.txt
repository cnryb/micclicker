idf_component_register(
    SRCS "src/mic_radio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_common
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
