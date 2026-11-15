idf_component_register(
    SRCS "src/mic_key_scan.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)

# The scanner is host-testable (firmware/tests/key_scan), so it must build warning free
# under the same flags the firmware uses.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
