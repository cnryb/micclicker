#pragma once

// ESP-NOW probe: sender role (C3) and receiver role (S3).
//
// Both roles share the frame codec, the radio layer and the counters; they differ in
// which traffic they generate and what they log. The probe is intentionally
// single-peer and single-threaded around one FreeRTOS task, because a measurement
// instrument that is itself racy cannot attribute loss to the radio.

#include "esp_err.h"
#include "mic_link.h"
#include "mic_radio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Channel used when the build does not override it. Both ends must agree; the probe
// logs the value it actually ended up on so a mismatch is visible in the two logs
// rather than only as silence.
#ifndef MIC_PROBE_CHANNEL
#define MIC_PROBE_CHANNEL MIC_RADIO_DEFAULT_CHANNEL
#endif

// Delay before a role starts its phases, so a host can attach a console to both ends
// after the reset that started them. Without it the opening lines are lost, and the
// opening lines are where the session and channel that define the run are reported.
#ifndef MIC_PROBE_START_DELAY_MS
#define MIC_PROBE_START_DELAY_MS 8000
#endif

// Role entry points. Each runs its own task and returns from app_main immediately.
esp_err_t mic_probe_run_sender(uint8_t channel);
esp_err_t mic_probe_run_receiver(uint8_t channel);

#ifdef __cplusplus
}
#endif
