idf_component_register(
    SRCS "src/mic_session.c"
    INCLUDE_DIRS "include"
    REQUIRES link_proto link_radio link_bind esp_timer esp_common
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
