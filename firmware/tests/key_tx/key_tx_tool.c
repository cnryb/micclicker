// Host-side test of the C3 key delivery state machine.
//
// The cases are the failure modes docs/protocol.md section 3.2 exists to prevent: an event
// lost because its ACK was late, an event applied twice because the retransmission was not
// recognised, and an attempt wasted on a send the radio refused.

#include <stdio.h>
#include <string.h>

#include "mic_key_tx.h"

static int s_checks;
static int s_failures;

static void check(int ok, const char *label)
{
    ++s_checks;
    if (!ok) {
        ++s_failures;
        printf("  FAIL %s\n", label);
    }
}

#define RETRY_MS 60
#define MAX_ATTEMPTS 4

static void fresh(mic_key_tx_t *tx)
{
    mic_key_tx_init(tx, MAX_ATTEMPTS, RETRY_MS);
}

static void test_first_transmission(void)
{
    printf("== first transmission ==\n");
    mic_key_tx_t tx;
    fresh(&tx);
    mic_key_tx_due_t due[MIC_KEY_TX_PENDING_MAX];

    uint32_t seq = mic_key_tx_submit(&tx, 0x03, 0x01);
    check(seq == 1, "first event gets sequence 1 (0 is reserved for 'dropped')");
    check(mic_key_tx_pending(&tx) == 1, "one event pending");

    // A new event is due immediately: the key has already been pressed, so waiting a retry
    // interval before the first transmission would add latency for no benefit.
    check(mic_key_tx_collect_due(&tx, 0, due, MIC_KEY_TX_PENDING_MAX) == 1, "new event is due at once");
    check(due[0].seq == seq, "due event carries its sequence");
    check(due[0].key_id == 0x03 && due[0].event == 0x01, "due event carries its payload");
    check(due[0].attempts == 1, "first collection counts as attempt 1");

    check(mic_key_tx_collect_due(&tx, 10, due, MIC_KEY_TX_PENDING_MAX) == 0, "not due again before the retry interval");
    check(mic_key_tx_collect_due(&tx, RETRY_MS - 1, due, MIC_KEY_TX_PENDING_MAX) == 0, "still not due just before it");
}

static void test_retransmission_same_seq(void)
{
    printf("== retransmission reuses the sequence ==\n");
    mic_key_tx_t tx;
    fresh(&tx);
    mic_key_tx_due_t due[MIC_KEY_TX_PENDING_MAX];

    uint32_t seq = mic_key_tx_submit(&tx, 0x01, 0x01);
    mic_key_tx_collect_due(&tx, 0, due, 1);

    check(mic_key_tx_collect_due(&tx, RETRY_MS, due, 1) == 1, "retransmission is due after the interval");
    check(due[0].seq == seq, "retransmission keeps the same sequence number");
    check(due[0].attempts == 2, "second collection is attempt 2");

    mic_key_tx_stats_t stats;
    mic_key_tx_get_stats(&tx, &stats);
    check(stats.transmissions == 2, "two transmissions counted");
    check(stats.retransmits == 1, "one of them counted as a retransmission");
}

static void test_ack_clears(void)
{
    printf("== ACK clears exactly one event ==\n");
    mic_key_tx_t tx;
    fresh(&tx);

    uint32_t first = mic_key_tx_submit(&tx, 0x01, 0x01);
    uint32_t second = mic_key_tx_submit(&tx, 0x01, 0x00);
    check(first != second, "consecutive events get different sequences");
    check(mic_key_tx_pending(&tx) == 2, "two events pending");

    check(mic_key_tx_ack(&tx, first), "ACK for the first event clears it");
    check(mic_key_tx_pending(&tx) == 1, "only that event was cleared");
    check(!mic_key_tx_ack(&tx, first), "a second ACK for the same event is stray");
    check(mic_key_tx_ack(&tx, second), "ACK for the pending event still works");

    mic_key_tx_stats_t stats;
    mic_key_tx_get_stats(&tx, &stats);
    check(stats.acknowledged == 2, "two events acknowledged");
    check(stats.stray_acks == 1, "the repeated ACK is counted as stray");
}

static void test_ack_by_number_not_order(void)
{
    printf("== ACKs match by number, not arrival order ==\n");
    mic_key_tx_t tx;
    fresh(&tx);

    uint32_t a = mic_key_tx_submit(&tx, 0x01, 0x01);
    uint32_t b = mic_key_tx_submit(&tx, 0x03, 0x01);

    // The receiver's ACK for the second event arrives first (its owns report landed
    // sooner). Matching by arrival order would clear the wrong event.
    check(mic_key_tx_ack(&tx, b), "out-of-order ACK matches its own event");
    check(mic_key_tx_pending(&tx) == 1, "the first event is still pending");
    check(mic_key_tx_ack(&tx, a), "the remaining event is still clearable by its number");
}

static void test_expiry(void)
{
    printf("== attempt limit ==\n");
    mic_key_tx_t tx;
    fresh(&tx);
    mic_key_tx_due_t due[MIC_KEY_TX_PENDING_MAX];

    uint32_t seq = mic_key_tx_submit(&tx, 0x03, 0x01);
    (void)seq;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        int64_t now = (int64_t)attempt * RETRY_MS;
        check(mic_key_tx_collect_due(&tx, now, due, 1) == 1, "attempt is due inside the limit");
    }
    check(mic_key_tx_collect_due(&tx, RETRY_MS * MAX_ATTEMPTS, due, 1) == 0,
          "no further attempt past the limit");

    mic_key_tx_expire(&tx);
    check(mic_key_tx_pending(&tx) == 0, "expired event is removed");

    mic_key_tx_stats_t stats;
    mic_key_tx_get_stats(&tx, &stats);
    check(stats.expired == 1, "expiry counted");
    check(stats.transmissions == MAX_ATTEMPTS, "exactly the attempt limit was transmitted");
}

static void test_undefer(void)
{
    printf("== refused send does not spend an attempt ==\n");
    mic_key_tx_t tx;
    fresh(&tx);
    mic_key_tx_due_t due[MIC_KEY_TX_PENDING_MAX];

    uint32_t seq = mic_key_tx_submit(&tx, 0x01, 0x01);
    check(mic_key_tx_collect_due(&tx, 0, due, 1) == 1, "attempt 1 is due");

    // The radio refused the send, so the frame never reached the air.
    mic_key_tx_undefer(&tx, seq, 0);

    mic_key_tx_stats_t stats;
    mic_key_tx_get_stats(&tx, &stats);
    check(stats.transmissions == 0, "the refused attempt is not counted as a transmission");

    // It is due again immediately, not after another retry interval: nothing was sent.
    check(mic_key_tx_collect_due(&tx, 1, due, 1) == 1, "retry is due immediately");
    check(due[0].attempts == 1, "the retry is still attempt 1");

    // Repeated refusals must not silently exhaust the attempt limit.
    for (int i = 0; i < 10; ++i) {
        mic_key_tx_collect_due(&tx, 100 + i, due, 1);
        mic_key_tx_undefer(&tx, seq, 100 + i);
    }
    mic_key_tx_expire(&tx);
    check(mic_key_tx_pending(&tx) == 1, "a busy queue does not expire the event");
}

static void test_queue_bound(void)
{
    printf("== bounded queue ==\n");
    mic_key_tx_t tx;
    fresh(&tx);

    for (int i = 0; i < MIC_KEY_TX_PENDING_MAX; ++i) {
        check(mic_key_tx_submit(&tx, 0x01, 0x01) != 0, "event accepted while the queue has room");
    }
    check(mic_key_tx_pending(&tx) == MIC_KEY_TX_PENDING_MAX, "queue is full");

    uint32_t overflow = mic_key_tx_submit(&tx, 0x03, 0x01);
    check(overflow != 0, "the overflowing event is still assigned a number");
    check(mic_key_tx_pending(&tx) == MIC_KEY_TX_PENDING_MAX, "the queue stays bounded");

    mic_key_tx_stats_t stats;
    mic_key_tx_get_stats(&tx, &stats);
    check(stats.dropped == 1, "the dropped event is counted");
    check(stats.submitted == MIC_KEY_TX_PENDING_MAX + 1, "it still counts as submitted");
}

static void test_reset(void)
{
    printf("== reset ==\n");
    mic_key_tx_t tx;
    fresh(&tx);
    mic_key_tx_due_t due[MIC_KEY_TX_PENDING_MAX];

    mic_key_tx_submit(&tx, 0x01, 0x01);
    mic_key_tx_submit(&tx, 0x03, 0x01);
    mic_key_tx_reset(&tx);
    check(mic_key_tx_pending(&tx) == 0, "reset drops every pending event");
    check(mic_key_tx_collect_due(&tx, 1000, due, 1) == 0, "nothing is due after a reset");

    // Counters survive a reset: they are cumulative evidence for the run, not queue state.
    mic_key_tx_stats_t stats;
    mic_key_tx_get_stats(&tx, &stats);
    check(stats.submitted == 2, "submitted counter survives reset");
}

static void test_mixed_traffic(void)
{
    printf("== realistic press/release sequence ==\n");
    mic_key_tx_t tx;
    fresh(&tx);
    mic_key_tx_due_t due[MIC_KEY_TX_PENDING_MAX];

    // One full press/release pair, acknowledged as each lands.
    uint32_t press = mic_key_tx_submit(&tx, 0x01, 0x01);
    check(mic_key_tx_collect_due(&tx, 0, due, 1) == 1, "press sent");
    check(mic_key_tx_ack(&tx, press), "press acknowledged");

    uint32_t release = mic_key_tx_submit(&tx, 0x01, 0x00);
    check(release != press, "release gets its own sequence");
    check(mic_key_tx_collect_due(&tx, 100, due, 1) == 1, "release sent");
    check(mic_key_tx_ack(&tx, release), "release acknowledged");
    check(mic_key_tx_pending(&tx) == 0, "nothing left pending");

    mic_key_tx_stats_t stats;
    mic_key_tx_get_stats(&tx, &stats);
    check(stats.submitted == 2, "two events submitted");
    check(stats.acknowledged == 2, "two events acknowledged");
    check(stats.transmissions == 2, "no retransmission was needed");
}

static void test_null_safety(void)
{
    printf("== null safety ==\n");
    mic_key_tx_due_t due[MIC_KEY_TX_PENDING_MAX];
    mic_key_tx_init(NULL, MAX_ATTEMPTS, RETRY_MS);
    mic_key_tx_reset(NULL);
    mic_key_tx_expire(NULL);
    check(mic_key_tx_submit(NULL, 0x01, 0x01) == 0, "null submit drops");
    check(mic_key_tx_collect_due(NULL, 0, due, 1) == 0, "null collect returns nothing");
    check(!mic_key_tx_ack(NULL, 1), "null ack is false");
    check(mic_key_tx_pending(NULL) == 0, "null pending is zero");
    mic_key_tx_get_stats(NULL, NULL);
}

int main(void)
{
    printf("== key_tx host tests ==\n\n");
    test_first_transmission();
    test_retransmission_same_seq();
    test_ack_clears();
    test_ack_by_number_not_order();
    test_expiry();
    test_undefer();
    test_queue_bound();
    test_reset();
    test_mixed_traffic();
    test_null_safety();

    printf("\n%d checks, %d failures\n", s_checks, s_failures);
    if (s_failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL\n");
    return 1;
}
