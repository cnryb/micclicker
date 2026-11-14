idf_component_register(
    SRCS "app_main.c" "stage1_selftest.c" "usb_prototype.c" "usb_descriptors.c"
    INCLUDE_DIRS "."
    REQUIRES esp_psram heap esp_hw_support esp_driver_gpio esp_timer wifi_scan esp_tinyusb link_proto link_radio link_bind link_probe
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
