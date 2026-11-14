idf_component_register(
    SRCS "src/mic_bind.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_common link_proto
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
