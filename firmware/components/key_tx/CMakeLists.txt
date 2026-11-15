idf_component_register(
    SRCS "src/mic_key_tx.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)

# The delivery state machine has no ESP-IDF dependency beyond the error type, so it is
# host-testable (firmware/tests/key_tx) with the same warning flags as the firmware.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
