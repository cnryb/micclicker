cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# UAC2 microphone: 16 kHz, signed 16-bit little-endian PCM, one channel.
# TinyUSB reserves one extra sample per USB frame for its IN flow control.
idf_build_set_property(COMPILE_DEFINITIONS
    "CFG_TUD_AUDIO=1;CFG_TUD_AUDIO_ENABLE_EP_IN=1;CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL=1;CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE=16000;CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX=2;CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX=1;CFG_TUD_AUDIO_EP_SZ_IN=34;CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX=34;CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ=272"
    APPEND)

set(PROJECT_VER "0.2.0-usb-prototype")
project(micclicker_s3)
