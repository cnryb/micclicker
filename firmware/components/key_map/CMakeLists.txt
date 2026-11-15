idf_component_register(
    SRCS "src/mic_key_map.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)

# Mapping validation and report encoding are host-testable (firmware/tests/key_map), so
# the component must build warning free under the firmware's flags.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
