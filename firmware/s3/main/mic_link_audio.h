#pragma once

// Bridging surface between the S3 link receiver and the USB audio device.
//
// The receiver owns the ring and the decode state; the USB prototype owns the UAC
// endpoint and therefore decides when audio is actually consumed. This header is the
// only contract between them, so the USB side never needs to know about ESP-NOW,
// G.722 or sessions, and the link side never needs to know about endpoints.

#include <stdint.h>

#include "mic_jitter.h"
#include "mic_link.h"

#ifdef __cplusplus
extern "C" {
#endif

// Counters the USB status report exposes, so a host tool can see link health without
// attaching a console to the CH343 port.
typedef struct {
    uint32_t audio_rx;             // audio frames accepted (duplicates included)
    uint32_t audio_dup;            // duplicates discarded
    uint32_t audio_gap;            // sequence numbers inferred missing
    uint32_t audio_late;           // frames that arrived after their gap was counted
    uint32_t audio_decoded;        // frames decoded into PCM
    uint32_t audio_decode_errors;  // decode failures
    uint32_t rejected;             // frames rejected before dispatch
    uint32_t heartbeats;           // heartbeats received
} mic_link_audio_stats_t;

// Bring up the receiver: radio, binding, decoder and ring. Then start the status task.
// Call once, after TinyUSB is installed, so the first UAC read already has a ring.
void micclicker_s3_audio_start(void);

// The ring the UAC endpoint drains. Never NULL after micclicker_s3_audio_start.
mic_jitter_t *mic_link_audio_ring(void);

// Consistent snapshot of the link counters.
void mic_link_audio_get_stats(mic_link_audio_stats_t *out);

// Tell the receiver whether the host is draining the endpoint (UAC alternative setting 1
// and mounted). While it is not, decoded frames are counted but not queued, so the ring
// does not accumulate a backlog that would be reported as overflow. A change in either
// direction flushes the ring so stale audio is never played.
void mic_link_audio_set_consuming(bool consuming);

// Park one heartbeat for the product task to answer. Called from the receive callback,
// which must not send: answering from there re-enters ESP-NOW inside its own callback.
// `seq` is the sender's heartbeat sequence number, which the reply has to echo.
void mic_link_audio_note_heartbeat(uint32_t seq, const mic_link_heartbeat_t *beat);

#ifdef __cplusplus
}
#endif
