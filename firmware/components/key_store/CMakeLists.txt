idf_component_register(
    SRCS "src/mic_key_store.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common nvs_flash key_map
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
