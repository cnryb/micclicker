#pragma once

// MicClicker application-layer frame format for the ESP-NOW link.
//
// The wire layout is fixed by docs/DESIGN.md section 4.2 and expanded in
// docs/protocol.md: an 8-byte common header followed by a per-type payload. All
// multi-byte fields are little-endian. Nothing here maps a C struct onto the wire;
// every field is written and read byte by byte so padding, alignment and host
// endianness can never leak into the protocol.
//
// ESP-NOW v1.0 caps each transmission at 250 bytes, so mic_link_frame_t is sized
// for that limit and every builder rejects an oversize result instead of truncating.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bump only together with docs/DESIGN.md and docs/protocol.md. A receiver drops
// every frame whose version differs, so a mismatch is a visible link failure
// rather than a silent reinterpretation of fields.
#define MIC_LINK_VERSION 1

#define MIC_LINK_HEADER_BYTES 8
// ESP_NOW_MAX_DATA_LEN for ESP-NOW v1.0.
#define MIC_LINK_MAX_FRAME_BYTES 250

// 64 kbit/s * 20 ms, one encoded G.722 frame per packet (DESIGN.md section 4.2).
#define MIC_LINK_AUDIO_PAYLOAD_BYTES 160
#define MIC_LINK_AUDIO_FRAME_BYTES (MIC_LINK_HEADER_BYTES + MIC_LINK_AUDIO_PAYLOAD_BYTES)

// Payload sizes, kept next to the wire layout in docs/protocol.md.
#define MIC_LINK_KEY_PAYLOAD_BYTES 2
#define MIC_LINK_KEY_FRAME_BYTES (MIC_LINK_HEADER_BYTES + MIC_LINK_KEY_PAYLOAD_BYTES)
#define MIC_LINK_KEY_ACK_PAYLOAD_BYTES 4
#define MIC_LINK_KEY_ACK_FRAME_BYTES (MIC_LINK_HEADER_BYTES + MIC_LINK_KEY_ACK_PAYLOAD_BYTES)
#define MIC_LINK_HEARTBEAT_PAYLOAD_BYTES 4
#define MIC_LINK_HEARTBEAT_FRAME_BYTES (MIC_LINK_HEADER_BYTES + MIC_LINK_HEARTBEAT_PAYLOAD_BYTES)
#define MIC_LINK_HEARTBEAT_ACK_PAYLOAD_BYTES 36
#define MIC_LINK_HEARTBEAT_ACK_FRAME_BYTES (MIC_LINK_HEADER_BYTES + MIC_LINK_HEARTBEAT_ACK_PAYLOAD_BYTES)
// LMK(16) + PMK(16) + channel(1).
#define MIC_LINK_PAIR_REQ_PAYLOAD_BYTES 33
#define MIC_LINK_PAIR_REQ_FRAME_BYTES (MIC_LINK_HEADER_BYTES + MIC_LINK_PAIR_REQ_PAYLOAD_BYTES)
#define MIC_LINK_PAIR_RESP_PAYLOAD_BYTES 4
#define MIC_LINK_PAIR_RESP_FRAME_BYTES (MIC_LINK_HEADER_BYTES + MIC_LINK_PAIR_RESP_PAYLOAD_BYTES)

typedef enum {
    MIC_LINK_TYPE_AUDIO = 0x01,
    MIC_LINK_TYPE_KEY = 0x02,
    MIC_LINK_TYPE_KEY_ACK = 0x03,
    MIC_LINK_TYPE_HEARTBEAT = 0x04,
    MIC_LINK_TYPE_HEARTBEAT_ACK = 0x05,
    // Pairing control. These two types travel in the clear during pairing only,
    // because the sender's keys are not yet known to the receiver; see
    // docs/protocol.md for the security boundary this implies.
    MIC_LINK_TYPE_PAIR_REQ = 0x06,
    MIC_LINK_TYPE_PAIR_RESP = 0x07,
} mic_link_type_t;

// key_id values match the physical buttons; event distinguishes press from release.
// A single physical button therefore produces two KEY frames with distinct seqs.
typedef enum {
    MIC_LINK_KEY_UP = 0x01,
    MIC_LINK_KEY_DOWN = 0x02,
    MIC_LINK_KEY_OK = 0x03,
} mic_link_key_id_t;

typedef enum {
    MIC_LINK_KEY_EVENT_RELEASE = 0x00,
    MIC_LINK_KEY_EVENT_PRESS = 0x01,
} mic_link_key_event_t;

// HEARTBEAT carries a phase tag so a probe run can delimit test phases without
// adding a wire type, and `mode` so discovery can share the same type.
typedef enum {
    MIC_LINK_HEARTBEAT_MODE_DISCOVER = 0x00,
    MIC_LINK_HEARTBEAT_MODE_PROBE = 0x01,
} mic_link_heartbeat_mode_t;

// Link state bits reported by the receiver in HEARTBEAT_ACK.
#define MIC_LINK_STATE_PAIRED 0x01u

// PAIR_REQ carries the sender's freshly generated link keys plus the channel both
// ends must use. Generating them per pairing (rather than shipping a constant) keeps
// secrets out of the repository and out of the build, at the cost of a plaintext key
// handoff during the pairing window; docs/protocol.md records that limit.
#define MIC_LINK_PAIR_KEY_BYTES 16

typedef enum {
    MIC_LINK_PAIR_RESULT_OK = 0x00,
    MIC_LINK_PAIR_RESULT_REJECTED = 0x01,
    MIC_LINK_PAIR_RESULT_CHANNEL_MISMATCH = 0x02,
} mic_link_pair_result_t;

// The common header of every frame.
typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t session;
    uint32_t seq;
} mic_link_header_t;

// An encoded frame plus its exact length. `len` is always <= MIC_LINK_MAX_FRAME_BYTES.
typedef struct {
    uint8_t bytes[MIC_LINK_MAX_FRAME_BYTES];
    size_t len;
} mic_link_frame_t;

// A parsed frame. `payload` points into the caller's receive buffer, so a view is
// only valid while that buffer lives.
typedef struct {
    mic_link_header_t header;
    const uint8_t *payload;
    size_t payload_len;
} mic_link_view_t;

typedef struct {
    uint8_t key_id;
    uint8_t event;
} mic_link_key_t;

// Sender -> receiver. `phase` is opaque to the link layer; it only lets the probe
// logs correlate a phase across both ends.
typedef struct {
    uint8_t phase;
    uint8_t mode;
    uint16_t flags;
} mic_link_heartbeat_t;

typedef struct {
    uint8_t lmk[MIC_LINK_PAIR_KEY_BYTES];
    uint8_t pmk[MIC_LINK_PAIR_KEY_BYTES];
    uint8_t channel;
} mic_link_pair_req_t;

typedef struct {
    uint8_t result;
    uint8_t state;
    uint16_t reserved;
} mic_link_pair_resp_t;

// Receiver -> sender. Every counter is cumulative for the current session, so a
// sender computes per-phase figures by differencing two snapshots instead of
// requiring the receiver to reset anything. `phase` echoes the heartbeat that
// triggered this reply, which is what lets the sender attribute a snapshot to a
// probe phase.
typedef struct {
    // `echo_seq` is the seq of the heartbeat this reply answers. It is what makes the
    // reply usable as an RTT measurement: the sender matches it against its own
    // outstanding sends instead of guessing from arrival order.
    uint32_t echo_seq;
    uint8_t phase;
    uint8_t state;
    uint16_t flags;
    uint32_t audio_rx;     // accepted audio frames, duplicates included
    uint32_t audio_dup;    // audio frames discarded as duplicates
    uint32_t audio_gap;    // sequence numbers inferred missing
    uint32_t audio_late;   // frames that arrived after their gap was counted
    uint32_t key_rx;       // key events accepted (after dedup)
    uint32_t key_dup;      // key retransmissions suppressed by dedup
    uint32_t rejected;     // frames rejected before dispatch, any reason
} mic_link_heartbeat_ack_t;

// Total encoded length for a type, or 0 when the type is unknown.
size_t mic_link_frame_bytes(uint8_t type);

// Human-readable type name for logs; returns "unknown" for an unassigned value.
const char *mic_link_type_name(uint8_t type);

// Builders. Each writes the complete frame and sets frame->len. They return
// ESP_ERR_INVALID_ARG for a null pointer and ESP_ERR_INVALID_SIZE if the encoding
// would exceed MIC_LINK_MAX_FRAME_BYTES (which cannot currently happen, but keeps
// the builders safe if a payload grows).
esp_err_t mic_link_build_audio(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                               const uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES]);
esp_err_t mic_link_build_key(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                             const mic_link_key_t *key);
esp_err_t mic_link_build_key_ack(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                                 uint32_t acked_key_seq);
esp_err_t mic_link_build_heartbeat(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                                   const mic_link_heartbeat_t *heartbeat);
esp_err_t mic_link_build_heartbeat_ack(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                                       const mic_link_heartbeat_ack_t *ack);
esp_err_t mic_link_build_pair_req(mic_link_frame_t *frame, const mic_link_pair_req_t *request);
esp_err_t mic_link_build_pair_resp(mic_link_frame_t *frame, const mic_link_pair_resp_t *response);

// Validate an inbound frame and split it. Rejects, in this order:
// null pointer or oversize length (ESP_ERR_INVALID_ARG), short header
// (ESP_ERR_INVALID_SIZE), unknown version (ESP_ERR_INVALID_VERSION), unknown type
// (ESP_ERR_NOT_SUPPORTED), and a length that does not match the type exactly
// (ESP_ERR_INVALID_SIZE). Trailing bytes are rejected, not ignored, so a sender
// that appends data cannot silently pass as a valid frame.
esp_err_t mic_link_parse(const uint8_t *data, size_t len, mic_link_view_t *view);

// Typed payload decoders. They re-check the type and payload length so a caller
// that skips mic_link_parse's type dispatch still cannot misread a payload.
esp_err_t mic_link_decode_audio(const mic_link_view_t *view, const uint8_t **payload);
esp_err_t mic_link_decode_key(const mic_link_view_t *view, mic_link_key_t *key);
esp_err_t mic_link_decode_key_ack(const mic_link_view_t *view, uint32_t *acked_key_seq);
esp_err_t mic_link_decode_heartbeat(const mic_link_view_t *view, mic_link_heartbeat_t *heartbeat);
esp_err_t mic_link_decode_heartbeat_ack(const mic_link_view_t *view, mic_link_heartbeat_ack_t *ack);
esp_err_t mic_link_decode_pair_req(const mic_link_view_t *view, mic_link_pair_req_t *request);
esp_err_t mic_link_decode_pair_resp(const mic_link_view_t *view, mic_link_pair_resp_t *response);

// Per-type sequence accounting with wraparound handling.
//
// `holes` counts sequence numbers inferred missing at the moment a later frame
// arrives; `late` counts frames that show up after their hole was already counted.
// The two are reported separately because ESP-NOW does not reorder in practice, so
// a large `late` means the test deliberately permuted the order, while a large
// `holes` with `late` near zero means real loss. Effective loss is holes - late.
typedef struct {
    bool primed;
    uint32_t next;       // sequence number expected next
    uint32_t received;   // frames accepted, including duplicates
    uint32_t duplicates;
    uint32_t late;
    uint32_t holes;
    uint32_t largest_hole;
    uint32_t resyncs;    // times the tracker had to restart from a far-behind frame
    // Bit i means "sequence next-1-i was already received" for the last 64 frames.
    uint64_t recent;
} mic_link_seq_t;

#define MIC_LINK_SEQ_WINDOW 64

void mic_link_seq_reset(mic_link_seq_t *tracker);

// True when `seq` was already recorded within the recent window. Used to answer
// duplicate retransmissions (keys) and duplicates (audio) without applying them.
bool mic_link_seq_is_duplicate(const mic_link_seq_t *tracker, uint32_t seq);

// Account for one received sequence number and update every counter above.
void mic_link_seq_observe(mic_link_seq_t *tracker, uint32_t seq);

// Effective loss for this tracker: holes that were never filled by a late frame.
uint32_t mic_link_seq_missing(const mic_link_seq_t *tracker);

// Per-session dispatch state for one direction of the link.
//
// A session change resets every tracker, because sequence numbering restarts with
// the sender's boot and comparing across sessions would manufacture a huge hole.
// Only the newest observed session is honoured; a late frame from a superseded
// session after that switch would be taken as a new session, so callers that care
// about session flaps must filter on their own (the probe firmware does not).
typedef struct {
    bool session_valid;
    uint16_t session;
    uint32_t session_changes;
    mic_link_seq_t audio;
    mic_link_seq_t key;
} mic_link_session_t;

void mic_link_session_reset(mic_link_session_t *state);

// Adopt `session` if it differs from the current one. Returns true when the trackers
// were reset (first frame or a new session), false for an already-known session.
bool mic_link_session_adopt(mic_link_session_t *state, uint16_t session);

#ifdef __cplusplus
}
#endif
