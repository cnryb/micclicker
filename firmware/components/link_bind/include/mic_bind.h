#pragma once

// Persistent peer binding for the MicClicker link.
//
// DESIGN.md section 4.1 requires that a pairing survives reboots and that a normal
// reconnect never clears it. This module is the only writer of that record: it keeps
// the peer MAC, channel and link keys in a single NVS blob so a binding can never be
// half-updated (a partially written record would leave one end encrypted and the
// other end plaintext, which looks like a dead link rather than a pairing error).
//
// The keys are secrets. They are never logged, never printed, and never placed in the
// repository; they are generated at pairing time with esp_random().

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_now.h"
#include "mic_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool paired;
    uint8_t peer_mac[ESP_NOW_ETH_ALEN];
    uint8_t channel;
    uint8_t lmk[ESP_NOW_KEY_LEN];
    uint8_t pmk[ESP_NOW_KEY_LEN];
    // Bumped on every successful pairing so a stale copy of the record can be
    // recognised rather than silently trusted.
    uint32_t generation;
} mic_bind_record_t;

// Load the stored binding. Returns ESP_ERR_NOT_FOUND when no binding exists, which is
// the normal state of a factory-fresh unit and not an error to log loudly.
esp_err_t mic_bind_load(mic_bind_record_t *out);

// Store a binding atomically. Rejects an incomplete or invalid record instead of
// writing it, so a caller bug cannot persist a half-paired state.
esp_err_t mic_bind_store(const mic_bind_record_t *record);

// Remove the binding. Only an explicit unbind may call this.
esp_err_t mic_bind_erase(void);

// Validate a record's fields without touching NVS. Exposed so the probe and the
// product firmware apply the same rules to a record that came from elsewhere.
esp_err_t mic_bind_validate(const mic_bind_record_t *record);

#ifdef __cplusplus
}
#endif
