#pragma once

// Product-side link bring-up shared by both ends of the MicClicker audio link.
//
// The probe (firmware/components/link_probe) is a measurement instrument: it erases
// any stored binding on every boot and always runs the full discovery-then-pair
// schedule. The product cannot do that. It has to load the binding from NVS, come up
// encrypted on the stored channel without touching NVS, and only enter a pairing
// window when there is no binding or when the user explicitly asks to pair.
//
// That difference is the entire reason this component exists, so the distinction is
// stated here once: link_session never erases a binding on a normal boot
// (docs/protocol.md invariant 2).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mic_bind.h"
#include "mic_link.h"
#include "mic_radio.h"

#ifdef __cplusplus
extern "C" {
#endif

// How long a pairing window stays open once started. DESIGN.md section 4.1 requires
// pairing to happen inside a bounded window driven by a physical action; 30 s matches
// the probe and docs/protocol.md section 3.5.
#define MIC_SESSION_PAIR_WINDOW_MS 30000

// Interval between pairing requests inside the window. Long enough that the window
// does not saturate the channel, short enough that a receiver that was already
// listening catches the first or second request.
#define MIC_SESSION_PAIR_INTERVAL_MS 300

typedef enum {
    // No binding in NVS. The role has to open a pairing window.
    MIC_SESSION_STATE_UNBOUND = 0,
    // A binding was loaded and applied; encrypted unicast is usable.
    MIC_SESSION_STATE_READY,
    // A pairing window is open and pair requests are being broadcast.
    MIC_SESSION_STATE_PAIRING,
    // The last read of the binding failed for a reason other than "absent".
    MIC_SESSION_STATE_ERROR,
} mic_session_state_t;

// Bring the radio up on the stored channel (or the configured default when there is
// no binding) and apply the stored binding.
//
// On success the radio is started, the peer is registered as an encrypted unicast
// peer, and the position in the binding is available via mic_session_get().
//
// `fallback_channel` is used only when no binding exists; a stored channel always
// wins, because the peer records the channel it paired on and sending to the same
// peer on a different channel fails with ESP_ERR_ESPNOW_CHAN.
esp_err_t mic_session_start(uint8_t fallback_channel);

// Install the receive sink. Called after mic_session_start; the callback contract is
// mic_radio_recv_fn (Wi-Fi task context, must copy anything it keeps).
esp_err_t mic_session_set_receiver(mic_radio_recv_fn callback, void *context);

// Open a pairing window as the *initiator*. Generates fresh keys with esp_random(),
// broadcasts them (plaintext, per docs/protocol.md section 3.5), waits for the peer's
// response, then registers the peer as encrypted and stores the binding in NVS.
//
// Returns ESP_OK only when the peer accepted and the binding was persisted. Runs for
// at most MIC_SESSION_PAIR_WINDOW_MS.
esp_err_t mic_session_pair_as_initiator(uint8_t channel);

// Answer pairing requests as the *responder*. Blocks for `window_ms`, adopting the
// first request that matches `channel` and persisting the resulting binding.
//
// Passing 0 uses MIC_SESSION_PAIR_WINDOW_MS. Both ends must be pairing at the same
// time, which is what the physical pairing gesture is for.
//
// MIC_SESSION_PAIR_FOREVER keeps the window open until a request arrives. That is the
// right mode for a receiver that has nothing else to do while unbound: the two boards
// are reset independently, so a fixed window only works when the initiator happens to
// boot inside it. It is not used once a binding exists, because a bound receiver must
// not silently accept a new peer.
#define MIC_SESSION_PAIR_FOREVER UINT32_MAX
esp_err_t mic_session_pair_as_responder(uint32_t window_ms);

// Current binding plus state, for the status report and for callers that need the
// peer MAC before sending.
typedef struct {
    mic_session_state_t state;
    bool paired;
    uint8_t peer_mac[ESP_NOW_ETH_ALEN];
    uint8_t channel;
    uint32_t generation;
} mic_session_info_t;

void mic_session_get(mic_session_info_t *out);

// True once an encrypted unicast peer is registered and usable for product traffic.
bool mic_session_ready(void);

// Forget the binding and drop the peer. Only an explicit unbind may call this; a
// normal boot must never reach it (docs/protocol.md invariant 2).
esp_err_t mic_session_unbind(void);

// Send one already-built frame to the bound peer over the encrypted unicast path.
// Returns ESP_ERR_INVALID_STATE when no peer is bound, so a caller cannot mistake an
// unpaired link for a radio failure.
esp_err_t mic_session_send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
