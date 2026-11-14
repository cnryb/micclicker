#pragma once

// Host-side stand-in for ESP-IDF's esp_err.h.
//
// The G.722 component draws its error codes from esp_err_t so firmware callers see
// one error convention. Building the codec for the host test harness would
// otherwise require a full ESP-IDF install, so this shim reproduces exactly the
// values in $IDF_PATH/components/esp_common/include/esp_err.h. It is reachable only
// from firmware/tests/g722 (which puts this directory first on the include path);
// firmware builds resolve the real IDF header. Keep the values in sync with IDF.
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                  (-1)

#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_INVALID_RESPONSE  0x108
#define ESP_ERR_INVALID_CRC       0x109
#define ESP_ERR_INVALID_VERSION   0x10A
#define ESP_ERR_INVALID_MAC       0x10B
#define ESP_ERR_NOT_FINISHED      0x10C
#define ESP_ERR_NOT_ALLOWED       0x10D
