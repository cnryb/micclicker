#include "mic_g722.h"

#include <stddef.h>
#include <string.h>

#include "g722_enc_dec.h"

// The wrapper keeps the codec state as an aligned byte array in the public header
// so callers can allocate it by value without including the vendored types. These
// assertions are the contract between that array and the real codec structs: if a
// future upstream change grows either state past the reservation, the build fails
// instead of overflowing caller memory.
_Static_assert(sizeof(G722EncoderState) <= sizeof(((mic_g722_encoder_t *)0)->opaque),
               "mic_g722_encoder_t opaque buffer is too small for G722EncoderState");
_Static_assert(sizeof(G722DecoderState) <= sizeof(((mic_g722_decoder_t *)0)->opaque),
               "mic_g722_decoder_t opaque buffer is too small for G722DecoderState");
_Static_assert(_Alignof(G722EncoderState) <= _Alignof(mic_g722_encoder_t),
               "mic_g722_encoder_t alignment is too small for G722EncoderState");
_Static_assert(_Alignof(G722DecoderState) <= _Alignof(mic_g722_decoder_t),
               "mic_g722_decoder_t alignment is too small for G722DecoderState");

// `rate` selects the codec bit rate, not the PCM sample rate: 48000/56000/64000
// map to 6/7/8 bits per sample and anything else silently falls through to 8.
// 64000 wideband is the only rate DESIGN.md allows, so pass it verbatim.
#define MIC_G722_BITRATE 64000
// No G722_SAMPLE_RATE_8000 (we feed real 16 kHz) and no G722_PACKED (packing only
// applies below 64 kbit/s; at 64 the codec forces a byte per sample pair anyway).
#define MIC_G722_OPTIONS 0

static G722EncoderState *encoder_state(mic_g722_encoder_t *state)
{
    return (G722EncoderState *)(void *)state->opaque;
}

static G722DecoderState *decoder_state(mic_g722_decoder_t *state)
{
    return (G722DecoderState *)(void *)state->opaque;
}

esp_err_t mic_g722_encoder_init(mic_g722_encoder_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));
    if (WebRtc_g722_encode_init(encoder_state(state), MIC_G722_BITRATE, MIC_G722_OPTIONS) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    state->initialized = true;
    return ESP_OK;
}

esp_err_t mic_g722_decoder_init(mic_g722_decoder_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));
    WebRtc_g722_decode_init(decoder_state(state), MIC_G722_BITRATE, MIC_G722_OPTIONS);
    state->initialized = true;
    return ESP_OK;
}

esp_err_t mic_g722_encode(mic_g722_encoder_t *state,
                          const int16_t samples[MIC_G722_FRAME_SAMPLES],
                          uint8_t out[MIC_G722_FRAME_BYTES])
{
    if (state == NULL || samples == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!state->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    // The codec writes one byte per two samples at 64 kbit/s; anything other than
    // a full frame would silently change the packet size, so it is rejected.
    size_t written = WebRtc_g722_encode(encoder_state(state), out, samples, MIC_G722_FRAME_SAMPLES);
    return (written == MIC_G722_FRAME_BYTES) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t mic_g722_decode(mic_g722_decoder_t *state,
                          const uint8_t in[MIC_G722_FRAME_BYTES],
                          int16_t samples[MIC_G722_FRAME_SAMPLES])
{
    if (state == NULL || in == NULL || samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!state->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t written = WebRtc_g722_decode(decoder_state(state), samples, in, MIC_G722_FRAME_BYTES);
    return (written == MIC_G722_FRAME_SAMPLES) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
