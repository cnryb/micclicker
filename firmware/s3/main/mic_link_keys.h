#pragma once

// S3-side key path: turn acknowledged key events into standard HID keyboard reports.
//
// The shape of this module is set by one constraint: the ESP-NOW receive callback runs in
// the Wi-Fi task and must not send, log or allocate. The callback therefore only queues a
// validated event, and everything else happens here on a task that can block:
//
//   KEY frame (Wi-Fi task)  ->  queue  ->  dedup already done by link_proto
//                                        ->  apply to key state
//                                        ->  HID report (retried until the host polls)
//                                        ->  KEY_ACK back to the sender
//
// The ACK is sent only after the report that the event implies has actually been handed
// to the USB stack. Acknowledging an event whose report never reached the host would
// silently lose the keystroke, and the sender's retransmission could not repair it: the
// sequence number has already been applied, so the retry would be suppressed as a
// duplicate.
//
// Every path that can invalidate the host's view of the keys (link timeout, new session,
// USB reset, mapping change) ends in mic_link_keys_release_all(), which forces one
// all-zero report (AGENTS.md section 4, "所有按键释放时发送全零键盘报告").

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mic_key_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t events_rx;       // key events queued by the link callback
    uint32_t events_applied;  // events that changed the held set or produced a report
    uint32_t duplicates;      // suppressed by sequence dedup before reaching this module
    uint32_t reports;         // HID reports handed to the USB stack
    uint32_t reports_retried; // report attempts that had to wait for the host
    uint32_t acks_sent;       // KEY_ACK frames transmitted
    uint32_t ack_failures;    // KEY_ACK transmissions the radio refused
    uint32_t all_up;          // all-keys-up actions performed
    uint32_t timeouts;        // link-timeout driven all-keys-up
    uint32_t dropped;         // events discarded because the queue was full
    uint32_t invalid;         // events rejected (unknown key id / event value)
    uint32_t pending;         // events queued but not yet acknowledged
    uint8_t map_generation;   // bumped on every successful mapping change
} mic_link_key_stats_t;

// Load the stored mapping (or the defaults when none is stored), then start the task that
// drains the event queue. Call after the USB device is installed so reports have
// somewhere to go.
esp_err_t micclicker_s3_keys_start(void);

// Queue one key event for the task. Called from the ESP-NOW receive callback, so this
// must not block: a full queue drops the event and counts it. `session` is the session the
// frame arrived under; the KEY_ACK has to echo it (docs/protocol.md section 2).
void mic_link_keys_on_event(uint16_t session, uint32_t seq, uint8_t key_id, uint8_t event);

// Record one retransmission suppressed by the link's sequence dedup. The frame never
// reaches this module, so the counter has to be reported from the callback side.
void mic_link_keys_note_duplicate(void);

// Apply an event that did not come from the link (the stage-2 config-HID self-test
// commands). It goes through the same state machine and report path so there is exactly
// one writer of the keyboard report, and it carries no sequence number, so nothing is
// acknowledged. `seq == 0` is what marks an event as locally generated inside
// mic_link_keys_on_event.
void mic_link_keys_self_test(uint8_t key_id, uint8_t event);

// Force every key up and push the all-zero report. Safe to call from any task; the actual
// report is emitted by the key task so USB calls never happen from a callback.
void mic_link_keys_release_all(const char *reason);

// The session changed (the sender rebooted): sequence numbering restarted, so any event
// still queued belongs to a dead session and must not be applied.
void mic_link_keys_on_session_change(void);

// The link has been silent long enough that the sender cannot be trusted to send the
// matching releases.
void mic_link_keys_on_link_lost(void);

// USB reset, detach or resume: the host's view of the keyboard cannot be assumed any
// more, so the held state is released and re-sent.
void mic_link_keys_on_usb_reset(void);

// Validate, release all keys, persist and adopt a new mapping. Rejects an invalid table
// before anything is released or written, so a bad configuration leaves the device in the
// state it was in.
esp_err_t mic_link_keys_apply_map(const mic_key_map_t *map);

// Restore the design defaults (UP = Right Alt, DOWN = reserved, OK = Enter) and persist.
esp_err_t mic_link_keys_reset_map(void);

const mic_key_map_t *mic_link_keys_map(void);

// Emit one log line describing the active mapping, in the same shape on every path that
// changes it so a capture can be diffed across boots.
void mic_link_keys_log_map(const char *reason);

void mic_link_keys_get_stats(mic_link_key_stats_t *out);

// Bitmask of the physical keys currently held (1u << key_id). Exposed for the status line
// so a stuck key is visible in a capture instead of only being felt at the keyboard.
uint8_t mic_link_keys_down_mask(void);

// True when nothing is held and nothing is queued. Used by the status line.
bool mic_link_keys_idle(void);

#ifdef __cplusplus
}
#endif
