#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Run one blocking 2.4 GHz STA scan. SSIDs and BSSIDs are deliberately not logged.
// NVS is initialized when needed, but is never erased or repaired implicitly.
esp_err_t micclicker_wifi_scan_run(void);

#ifdef __cplusplus
}
#endif
