#include "mic_key_tx.h"

#include <string.h>

// Slot indices are physical, so `due_ms == 0` can be used as "not yet transmitted"
// without colliding with a real deadline: the caller's monotonic clock is milliseconds
// since boot and a first transmission is always due immediately.
#define DUE_IMMEDIATELY 0

static mic_key_tx_slot_t *slot_at(mic_key_tx_t *tx, size_t offset)
{
    return &tx->slots[(tx->head + offset) % MIC_KEY_TX_PENDING_MAX];
}

void mic_key_tx_init(mic_key_tx_t *tx, uint32_t max_attempts, uint32_t retry_ms)
{
    if (tx == NULL) {
        return;
    }
    memset(tx, 0, sizeof(*tx));
    tx->max_attempts = max_attempts == 0 ? 1 : max_attempts;
    tx->retry_ms = retry_ms;
    // 0 is reserved as "dropped", so the first real event is 1.
    tx->next_seq = 1;
}

void mic_key_tx_reset(mic_key_tx_t *tx)
{
    if (tx == NULL) {
        return;
    }
    tx->head = 0;
    tx->count = 0;
}

uint32_t mic_key_tx_submit(mic_key_tx_t *tx, uint8_t key_id, uint8_t event)
{
    if (tx == NULL) {
        return 0;
    }
    if (tx->count == MIC_KEY_TX_PENDING_MAX) {
        // Drop the oldest instead of the newest: a stale press is more dangerous than a
        // missing one, because the receiver would hold that key down.
        tx->head = (tx->head + 1) % MIC_KEY_TX_PENDING_MAX;
        --tx->count;
        ++tx->stats.dropped;
    }

    mic_key_tx_slot_t *slot = slot_at(tx, tx->count);
    slot->seq = tx->next_seq++;
    if (tx->next_seq == 0) {
        tx->next_seq = 1;
    }
    slot->key_id = key_id;
    slot->event = event;
    slot->attempts = 0;
    slot->due_ms = DUE_IMMEDIATELY;
    ++tx->count;
    ++tx->stats.submitted;
    return slot->seq;
}

static bool is_due(const mic_key_tx_t *tx, const mic_key_tx_slot_t *slot, int64_t now_ms)
{
    (void)tx;
    if (slot->attempts == 0) {
        return true;
    }
    return now_ms >= slot->due_ms;
}

size_t mic_key_tx_collect_due(mic_key_tx_t *tx, int64_t now_ms, mic_key_tx_due_t *out,
                              size_t max)
{
    if (tx == NULL || out == NULL || max == 0) {
        return 0;
    }
    size_t produced = 0;
    for (size_t offset = 0; offset < tx->count && produced < max; ++offset) {
        mic_key_tx_slot_t *slot = slot_at(tx, offset);
        if (!is_due(tx, slot, now_ms)) {
            continue;
        }
        if (slot->attempts >= tx->max_attempts) {
            // Exhausted; mic_key_tx_expire() removes it. Skipping here keeps the attempt
            // counter honest instead of wrapping it.
            continue;
        }
        if (slot->attempts > 0) {
            ++tx->stats.retransmits;
        }
        ++slot->attempts;
        ++tx->stats.transmissions;
        slot->due_ms = now_ms + (int64_t)tx->retry_ms;

        out[produced].seq = slot->seq;
        out[produced].key_id = slot->key_id;
        out[produced].event = slot->event;
        out[produced].attempts = slot->attempts;
        ++produced;
    }
    return produced;
}

void mic_key_tx_undefer(mic_key_tx_t *tx, uint32_t seq, int64_t now_ms)
{
    (void)now_ms;
    if (tx == NULL) {
        return;
    }
    for (size_t offset = 0; offset < tx->count; ++offset) {
        mic_key_tx_slot_t *slot = slot_at(tx, offset);
        if (slot->seq != seq) {
            continue;
        }
        // The attempt never reached the air, so it must not be spent. A refusal from the
        // driver queue is not evidence the receiver missed anything.
        if (slot->attempts > 0) {
            --slot->attempts;
            if (tx->stats.transmissions > 0) {
                --tx->stats.transmissions;
            }
            if (slot->attempts > 0 && tx->stats.retransmits > 0) {
                --tx->stats.retransmits;
            }
        }
        slot->due_ms = DUE_IMMEDIATELY;
        return;
    }
}

bool mic_key_tx_ack(mic_key_tx_t *tx, uint32_t acked_seq)
{
    if (tx == NULL || acked_seq == 0) {
        return false;
    }
    for (size_t offset = 0; offset < tx->count; ++offset) {
        mic_key_tx_slot_t *slot = slot_at(tx, offset);
        if (slot->seq != acked_seq) {
            continue;
        }
        // Remove by shifting the newer entries down one place. The queue is at most eight
        // entries, so this is cheaper than a linked list and keeps ordering explicit.
        for (size_t i = offset; i + 1 < tx->count; ++i) {
            *slot_at(tx, i) = *slot_at(tx, i + 1);
        }
        --tx->count;
        ++tx->stats.acknowledged;
        return true;
    }
    ++tx->stats.stray_acks;
    return false;
}

void mic_key_tx_expire(mic_key_tx_t *tx)
{
    if (tx == NULL) {
        return;
    }
    size_t offset = 0;
    while (offset < tx->count) {
        mic_key_tx_slot_t *slot = slot_at(tx, offset);
        if (slot->attempts < tx->max_attempts) {
            ++offset;
            continue;
        }
        for (size_t i = offset; i + 1 < tx->count; ++i) {
            *slot_at(tx, i) = *slot_at(tx, i + 1);
        }
        --tx->count;
        ++tx->stats.expired;
        // Do not advance: the next entry just shifted into this position.
    }
}

size_t mic_key_tx_pending(const mic_key_tx_t *tx)
{
    return tx == NULL ? 0 : tx->count;
}

void mic_key_tx_get_stats(const mic_key_tx_t *tx, mic_key_tx_stats_t *out)
{
    if (tx == NULL || out == NULL) {
        return;
    }
    *out = tx->stats;
}
