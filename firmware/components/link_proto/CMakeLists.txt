idf_component_register(
    SRCS "src/mic_link.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)

# The framing layer is host-testable (firmware/tests/link_proto), so it must build
# with no warnings under the same flags the firmware uses.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
