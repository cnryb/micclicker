idf_component_register(
    SRCS "src/mic_g722.c" "src/g722_encode.c" "src/g722_decode.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)

# The vendored SpanDSP/WebRTC core is third-party code kept byte-identical to
# upstream except for the include path. Warnings that the project rejects in its
# own sources must not force edits into upstream files.
set_source_files_properties(
    "src/g722_encode.c" "src/g722_decode.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare;-Wno-unused-parameter;-Wno-type-limits"
)
