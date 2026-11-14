#include "sdkconfig.h"

#if !CONFIG_MICCLICKER_STAGE1_SELFTEST && !CONFIG_MICCLICKER_ESPNOW_PROBE_RECEIVER

#include "usb_descriptors.h"

#include "class/hid/hid_device.h"

const tusb_desc_device_t mic_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303a,
    .idProduct = 0x4014,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_KEYBOARD,
    ITF_NUM_CONFIG,
    ITF_NUM_TOTAL,
};

const uint8_t mic_keyboard_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

const uint8_t mic_config_report_descriptor[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(MIC_CONFIG_REPORT_SIZE),
};

#define MIC_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO20_MIC_ONE_CH_DESC_LEN + \
                              TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

const uint8_t mic_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, MIC_CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR(
        ITF_NUM_AUDIO_CONTROL, 4, 2, 16, 0x81, CFG_TUD_AUDIO_EP_SZ_IN),

    TUD_HID_DESCRIPTOR(
        ITF_NUM_KEYBOARD, 5, HID_ITF_PROTOCOL_KEYBOARD,
        sizeof(mic_keyboard_report_descriptor), 0x82, 8, 10),

    TUD_HID_INOUT_DESCRIPTOR(
        ITF_NUM_CONFIG, 6, HID_ITF_PROTOCOL_NONE,
        sizeof(mic_config_report_descriptor), 0x03, 0x83, MIC_CONFIG_REPORT_SIZE, 10),
};

char mic_serial_descriptor[13] = "000000000000";

const char *mic_string_descriptor[MIC_STRING_DESCRIPTOR_COUNT] = {
    (const char[]){0x09, 0x04},
    "MicClicker",
    "MicClicker UAC2 and HID Prototype",
    mic_serial_descriptor,
    "MicClicker Microphone",
    "MicClicker Keyboard",
    "MicClicker Configuration",
};

#endif // !CONFIG_MICCLICKER_STAGE1_SELFTEST
