#pragma once

// Reliable delivery of debounced key events over an unreliable link.
//
// The rules come from docs/protocol.md section 3.2 and are the reason this is its own
// state machine rather than a few lines in the send loop:
//
//  * An unacknowledged event is retransmitted with the *same* sequence number until the
//    receiver acknowledges it or the attempt limit is reached. Reusing the number is what
//    lets the receiver's duplicate filter suppress the repeats.
//  * Only the receiver's ACK for that exact number clears it; arrival order is not
//    evidence, because a delayed ACK from an earlier event would otherwise release a
//    newer one.
//  * The queue is bounded. Keys are human-paced, so overflow means something is already
//    wrong; the oldest pending event is dropped and counted rather than growing without
//    limit.
//
// Nothing here formats bytes or touches a radio. It decides which event to transmit and
// when, which is what makes the retry and dedup behaviour testable on the host in
// firmware/tests/key_tx.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bounded on purpose. Human pressing cannot outrun eight in-flight events; if it does,
// the oldest is dropped and counted rather than the queue growing.
#define MIC_KEY_TX_PENDING_MAX 8

// Attempts per event, including the first transmission. Four is the same order as the
// probe used and covers a burst of ESP-NOW rejections without holding an event forever.
#define MIC_KEY_TX_MAX_ATTEMPTS 4

// Gap between attempts. The receiver answers ACKs from its product task, so 60 ms gives
// it several scheduling slots while still resolving a key well inside human perception.
#define MIC_KEY_TX_RETRY_MS 60

typedef struct {
    uint32_t seq;
    uint8_t key_id;
    uint8_t event;
    uint8_t attempts;   // transmissions already attempted
    // 0 when the event is waiting for its first transmission; otherwise the earliest
    // millisecond at which the next retransmission is due.
    int64_t due_ms;
} mic_key_tx_slot_t;

typedef struct {
    uint32_t seq;
    uint8_t key_id;
    uint8_t event;
    uint8_t attempts;
} mic_key_tx_due_t;

typedef struct {
    uint32_t submitted;    // events accepted from the scanner
    uint32_t transmissions;  // individual frames handed to the caller (incl. retries)
    uint32_t acknowledged;  // events cleared by a matching ACK
    uint32_t retransmits;   // transmissions that were not the first attempt
    uint32_t dropped;       // events discarded because the queue was full
    uint32_t expired;       // events dropped after exhausting the attempt limit
    uint32_t stray_acks;    // ACKs that matched nothing pending
} mic_key_tx_stats_t;

typedef struct {
    mic_key_tx_slot_t slots[MIC_KEY_TX_PENDING_MAX];
    size_t head;  // index of the oldest pending slot
    size_t count;
    uint32_t next_seq;
    uint32_t max_attempts;
    uint32_t retry_ms;
    mic_key_tx_stats_t stats;
} mic_key_tx_t;

void mic_key_tx_init(mic_key_tx_t *tx, uint32_t max_attempts, uint32_t retry_ms);

// Drop every pending event without emitting anything. Used when the link goes away: an
// event that could not be delivered is reported, not retried forever, and the receiver's
// own timeout is what releases the HID keys.
void mic_key_tx_reset(mic_key_tx_t *tx);

// Queue one debounced event and assign it a sequence number. Returns the sequence number
// used, or 0 when the event was dropped (queue full) — 0 is never a valid event number.
// A dropped *press* is followed by a dropped *release* only if the queue stays full, so
// the caller should surface the counter rather than ignore it.
uint32_t mic_key_tx_submit(mic_key_tx_t *tx, uint8_t key_id, uint8_t event);

// Collect the events that need transmitting now, oldest first, up to `max`.
//
// An event is due when it has never been transmitted or when its retry deadline has
// passed. Every returned item counts as one attempt; the caller transmits it and, if the
// radio refuses the send, calls mic_key_tx_defer() so the attempt is returned instead of
// being spent on a refusal that never reached the air.
size_t mic_key_tx_collect_due(mic_key_tx_t *tx, int64_t now_ms, mic_key_tx_due_t *out,
                              size_t max);

// Return an attempt that was not actually transmitted (radio refused). The event becomes
// due again immediately rather than waiting a full retry interval.
void mic_key_tx_undefer(mic_key_tx_t *tx, uint32_t seq, int64_t now_ms);

// Acknowledge one sequence number. Returns true when it cleared a pending event. An ACK
// for something already cleared counts as a stray ACK rather than an error: the reply
// can legitimately arrive twice when a later event's ACK was lost.
bool mic_key_tx_ack(mic_key_tx_t *tx, uint32_t acked_seq);

// Give up on events that have exhausted their attempt limit. Called from the same place
// as mic_key_tx_collect_due so expiry is observed even while nothing is being sent.
void mic_key_tx_expire(mic_key_tx_t *tx);

size_t mic_key_tx_pending(const mic_key_tx_t *tx);

void mic_key_tx_get_stats(const mic_key_tx_t *tx, mic_key_tx_stats_t *out);

#ifdef __cplusplus
}
#endif
