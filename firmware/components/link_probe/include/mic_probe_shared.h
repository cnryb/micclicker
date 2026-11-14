#pragma once

// Shared probe definition used by both ends of the ESP-NOW stage-3 test firmware.
//
// The probe is a deliberate one-time instrument, not product code: it exercises the
// link with deterministic traffic so loss, reordering, duplication and heartbeat RTT
// can be measured on real hardware, then logs one machine-readable summary per phase
// that a host script grades. It lives beside the product components so it can use
// exactly the same framing and radio code the product will use.

#include <stdbool.h>
#include <stdint.h>

#include "mic_link.h"

#ifdef __cplusplus
extern "C" {
#endif

// Probe phases, in execution order on the sender. The receiver reports whatever
// phase tag the sender puts in its heartbeats, so the two logs line up by tag.
typedef enum {
    PROBE_PHASE_IDLE = 0,
    PROBE_PHASE_DISCOVERY = 1,
    PROBE_PHASE_AUDIO_STREAM = 2,
    PROBE_PHASE_AUDIO_SPACED = 3,
    PROBE_PHASE_KEY_RETRY = 4,
    PROBE_PHASE_CHANNEL_SWEEP = 5,
    PROBE_PHASE_ENCRYPTED = 6,
    PROBE_PHASE_DONE = 7,
} probe_phase_t;

const char *probe_phase_name(uint8_t phase);

// Payload pattern shared by both ends.
//
// Audio payloads are not random: byte i of frame `seq` is a fixed function of seq and
// i, so the receiver can verify that a frame's contents belong to the sequence number
// in its header. That catches a corrupted frame that ESP-NOW's own CRC would already
// reject, and more usefully it catches a probe bug that reuses a buffer without
// refilling it.
#define PROBE_AUDIO_PATTERN(seq, i) ((uint8_t)(((seq) * 31u + (i) * 17u + 11u) & 0xFFu))

void probe_fill_audio(uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES], uint32_t seq);

// Verify a received audio payload against its seq. Returns the number of mismatching
// bytes, so 0 means the payload belongs to that seq.
uint32_t probe_check_audio(const uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES], uint32_t seq);

// The phase banner and per-phase summary lines both use this prefix so a host script
// can extract results without parsing human prose.
#define PROBE_TAG "PROBE"

#ifdef __cplusplus
}
#endif
