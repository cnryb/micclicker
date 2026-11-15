#pragma once

// C3 product role: physical keys in, key events on the link.
//
// The button hardware is a resistor ladder on one ADC pin (board.h), so this module owns
// three things that are easy to get wrong if they are mixed into the audio loop:
//
//  * sampling and classification of the ladder, on a fixed 10 ms cadence;
//  * debouncing, which is what turns chatter into one press and one release
//    (components/key_scan);
//  * delivery, which retransmits an unacknowledged event under the same sequence number
//    (components/key_tx).
//
// It also owns the sender's heartbeat. That is deliberate: the heartbeat is the only
// traffic that proves the link is alive when nobody is pressing keys, so it belongs to the
// task that already knows whether the radio is working, not to the audio path that is
// paced by the microphone.

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the ADC, the debouncer and the sender task. Call after the link session is up
// so the first event has somewhere to go; a key pressed before pairing is simply dropped,
// because the receiver never saw a session for it.
//
// `session` must be the same session number the audio path uses. Every frame on the link
// carries it, and the receiver resets its sequence trackers on any change, so a key frame
// claiming a different session would make the receiver flip-flop between "sessions" once
// per keystroke — re-running all-keys-up each time and defeating sequence dedup entirely.
esp_err_t micclicker_c3_keys_start(uint16_t session);

// The session changed (only possible before keys exist: the audio path assigns the number
// once at boot). Used by the key task when it starts before the session is assigned.
void micclicker_c3_keys_set_session(uint16_t session);

// Report a KEY_ACK received from the link. Called from the receive callback, so it must
// not block: it only clears a pending event.
void micclicker_c3_keys_on_ack(uint32_t acked_seq);

// Report a matched heartbeat reply. `echo_seq` is the heartbeat sequence number the reply
// answers, which is what makes it usable as a liveness and RTT signal instead of a guess.
void micclicker_c3_keys_on_heartbeat_ack(uint32_t echo_seq);

// One log line of cumulative counters, emitted once per second next to the audio status.
void micclicker_c3_keys_log_status(void);

#ifdef __cplusplus
}
#endif
