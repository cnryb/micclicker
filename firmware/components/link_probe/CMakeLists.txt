idf_component_register(
    SRCS "src/mic_probe_shared.c" "src/mic_probe_sender.c" "src/mic_probe_receiver.c"
    INCLUDE_DIRS "include"
    REQUIRES link_proto link_radio link_bind esp_wifi esp_timer esp_common
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
