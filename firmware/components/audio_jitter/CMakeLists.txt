idf_component_register(
    SRCS "src/mic_jitter.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)

# The ring is host-testable (firmware/tests/audio_jitter), so it must build warning
# free under the same flags the firmware uses.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
