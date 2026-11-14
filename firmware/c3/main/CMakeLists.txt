idf_component_register(
    SRCS "app_main.c" "g722_selftest.c"
    INCLUDE_DIRS "."
    REQUIRES esp_adc esp_hw_support esp_timer wifi_scan g722
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
