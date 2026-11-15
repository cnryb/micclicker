#pragma once

// Public surface of the C3 microphone capture path.
//
// This header intentionally exposes only what the audio task needs: bring the ES8311
// up, read exactly N mono samples, and change the analog gain. The I2S slot handling
// and the codec's register sequencing stay inside capture.c, so protocol or audio
// framing code never has to know how many slots the bus clocks.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The design baseline: 16 kHz, 16-bit signed little-endian, mono (DESIGN.md section 2).
#define MIC_CAPTURE_SAMPLE_RATE 16000
// One G.722 frame, which is also the audio task's read granularity.
#define MIC_CAPTURE_FRAME_SAMPLES 320
// Capture gain in dB. 30 dB was the value the previous project on this board used for
// voice at conversational distance; it is a starting point for the stage-4 run and is
// adjustable at runtime through mic_capture_set_gain().
#define MIC_CAPTURE_DEFAULT_GAIN_DB 30
// Samples pulled from the codec per read call. Small enough that a partial read still
// has bounded latency, large enough that the per-call overhead is negligible.
#define MIC_CAPTURE_CHUNK_SAMPLES 160

// Bring up I2C, I2S and the ES8311 in capture-only mode. Idempotent.
esp_err_t mic_capture_init(void);

bool mic_capture_ready(void);

// Read exactly `samples` mono samples. Blocks until the DMA has supplied them, so this
// must not be called from an ISR or from a callback that owes the Wi-Fi task a prompt
// return. Returns ESP_FAIL when the codec read fails; the caller should count it and
// keep the frame cadence rather than spinning.
esp_err_t mic_capture_read(int16_t *pcm, size_t samples);

// Set the analog input gain in dB, 0..42. Applied immediately when the codec is open,
// otherwise remembered for the next init.
esp_err_t mic_capture_set_gain(int gain_db);

int mic_capture_gain(void);

#ifdef __cplusplus
}
#endif
