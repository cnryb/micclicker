#pragma once

#include "tinyusb.h"

extern const tusb_desc_device_t mic_device_descriptor;
extern const uint8_t mic_configuration_descriptor[];
extern const uint8_t mic_keyboard_report_descriptor[];
extern const uint8_t mic_config_report_descriptor[];
extern const char *mic_string_descriptor[];
extern char mic_serial_descriptor[13];

#define MIC_STRING_DESCRIPTOR_COUNT 7
#define MIC_HID_KEYBOARD_INSTANCE 0
#define MIC_HID_CONFIG_INSTANCE 1
#define MIC_CONFIG_REPORT_ID 0
#define MIC_CONFIG_REPORT_SIZE 64
