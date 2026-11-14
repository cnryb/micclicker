#pragma once

// G.722 64 kbit/s wideband codec bound to the MicClicker audio frame.
//
// The codec core is the WebRTC third_party/g722 implementation of SpanDSP,
// which places its contributions in the public domain (see LICENSE-upstream.txt).
// It is an ITU G.722 implementation with a bit-exact option, so the framing below
// is the only thing this wrapper adds: one frame is 20 ms, 320 PCM samples in and
// 160 encoded bytes out.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 16 kHz * 20 ms. Every MacroFrame carries exactly one of these.
#define MIC_G722_FRAME_SAMPLES 320
// 64 kbit/s * 20 ms. One frame is always one packet (see DESIGN.md 4.2).
#define MIC_G722_FRAME_BYTES   160

// Encoder and decoder states are caller-owned so no allocation happens on the
// audio path. They are value types, so callers allocate them directly (statically
// or inside their own struct) rather than reaching for a size query.
typedef struct mic_g722_encoder {
    bool initialized;
    // Backing store for the codec state. Held as an aligned byte array so this
    // header need not pull in the vendored G.722 types or their size.
    _Alignas(8) unsigned char opaque[512];
} mic_g722_encoder_t;

typedef struct mic_g722_decoder {
    bool initialized;
    _Alignas(8) unsigned char opaque[512];
} mic_g722_decoder_t;

// Initialize at 64000 bit/s wideband. `state` stays owned by the caller. Safe to
// call again to reset a codec, which is what a new session needs so stale filter
// history cannot leak across it.
esp_err_t mic_g722_encoder_init(mic_g722_encoder_t *state);
esp_err_t mic_g722_decoder_init(mic_g722_decoder_t *state);

// Encode exactly MIC_G722_FRAME_SAMPLES mono samples into MIC_G722_FRAME_BYTES.
// Returns ESP_ERR_INVALID_ARG for a null pointer, ESP_ERR_INVALID_STATE when the
// codec was not initialized. Partial frames are rejected rather than padded.
esp_err_t mic_g722_encode(mic_g722_encoder_t *state,
                          const int16_t samples[MIC_G722_FRAME_SAMPLES],
                          uint8_t out[MIC_G722_FRAME_BYTES]);

// Decode exactly MIC_G722_FRAME_BYTES into MIC_G722_FRAME_SAMPLES mono samples.
// Same error contract as mic_g722_encode.
esp_err_t mic_g722_decode(mic_g722_decoder_t *state,
                          const uint8_t in[MIC_G722_FRAME_BYTES],
                          int16_t samples[MIC_G722_FRAME_SAMPLES]);

#ifdef __cplusplus
}
#endif
