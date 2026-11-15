#pragma once

// Persistent shortcut mapping for the S3 side.
//
// The mapping lives in its own NVS namespace, separate from the binding record
// (link_bind): a re-pairing must not disturb the user's keys, and a mapping change must
// not be able to damage the binding. Both are single blobs so an interrupted write leaves
// the previous record intact rather than a half-updated one.
//
// Only a validated table is ever written. Validation itself lives in key_map
// (mic_key_map_validate) so the same rules apply to the NVS load path, the config HID
// path and the host-side tests.

#include <stdbool.h>

#include "esp_err.h"
#include "mic_key_map.h"

#ifdef __cplusplus
extern "C" {
#endif

// Load the stored mapping. Returns ESP_ERR_NOT_FOUND when nothing is stored, which is the
// normal state of a factory-fresh unit; a blob that exists but does not decode returns the
// decoder's error so the caller can distinguish "absent" from "corrupt".
esp_err_t mic_key_store_load(mic_key_map_t *out);

// Validate and persist a mapping. Rejects an invalid table without writing anything.
esp_err_t mic_key_store_save(const mic_key_map_t *map);

// Remove the stored mapping, so the next load reports ESP_ERR_NOT_FOUND and the caller
// falls back to the design defaults.
esp_err_t mic_key_store_erase(void);

#ifdef __cplusplus
}
#endif
