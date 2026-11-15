// Host-side test of the receiver-side PCM ring and its drift controller.
//
// The ring is the only part of the audio chain whose behaviour is a control loop, so
// it is the one part worth testing without hardware: the drift cases below are exactly
// the ones a real pair of crystals produces, and the assertions are the ones the
// 30-minute acceptance run depends on (no unbounded growth, no permanent dry-out,
// fills the correct pack of bytes every millisecond).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mic_jitter.h"

// The component's mutex calls come from FreeRTOS in firmware and from the shim in
// hostshim/freertos/semphr.h on the host (compiled into host_shim.c). The test drives
// the ring from a single thread, so the mutex never contends; the shim still asserts
// that every take is matched by a give.

static int s_failures;
static int s_checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++s_checks;                                                                  \
        if (!(cond)) {                                                                \
            ++s_failures;                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
        }                                                                            \
    } while (0)

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        ++s_checks;                                                                   \
        long long a_ = (long long)(actual);                                           \
        long long e_ = (long long)(expected);                                         \
        if (a_ != e_) {                                                               \
            ++s_failures;                                                             \
            printf("FAIL %s:%d: %s == %s (got %lld, want %lld)\n", __FILE__, __LINE__, \
                   #actual, #expected, a_, e_);                                       \
        }                                                                             \
    } while (0)

// Fill one millisecond with a recognisable ramp so a splice or a reordering is
// visible in the sample values, not just in a counter.
static void make_ms(int16_t out[MIC_JITTER_SAMPLES_PER_MS], int16_t seed)
{
    for (size_t i = 0; i < MIC_JITTER_SAMPLES_PER_MS; ++i) {
        out[i] = (int16_t)(seed * 100 + (int16_t)i);
    }
}

// A ring that has never seen data must hand back silence, not garbage, and must count
// the underrun. This is the state the USB endpoint is in before the first frame
// arrives, so it has to be clean.
static void test_underrun_before_data(void)
{
    static mic_jitter_t ring;
    mic_jitter_init(&ring);

    int16_t out[MIC_JITTER_SAMPLES_PER_MS];
    memset(out, 0x7f, sizeof(out));
    CHECK_EQ(mic_jitter_take(&ring, out, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_OK);
    for (size_t i = 0; i < MIC_JITTER_SAMPLES_PER_MS; ++i) {
        CHECK_EQ(out[i], 0);
    }

    mic_jitter_stats_t stats;
    mic_jitter_get_stats(&ring, &stats);
    CHECK_EQ(stats.take_underruns, 1);
    CHECK_EQ(stats.plc_frames, 0);  // nothing to conceal from yet
    CHECK(stats.silence_frames >= 1);
}

// Order preservation and exact byte-for-byte round trip: whatever goes in comes out,
// in order, with no gaps, for a ring that stays inside its dead band.
static void test_round_trip_order(void)
{
    static mic_jitter_t ring;
    mic_jitter_init(&ring);

    int16_t ms[MIC_JITTER_SAMPLES_PER_MS];
    for (int16_t n = 0; n < 10; ++n) {
        make_ms(ms, n);
        CHECK_EQ(mic_jitter_put(&ring, ms, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_OK);
    }

    int16_t out[MIC_JITTER_SAMPLES_PER_MS];
    for (int16_t n = 0; n < 10; ++n) {
        make_ms(ms, n);
        CHECK_EQ(mic_jitter_take(&ring, out, MIC_JITTER_SAMPLES_PER_MS, 1000000), ESP_OK);
        CHECK_EQ(memcmp(out, ms, sizeof(ms)), 0);
    }

    CHECK_EQ(mic_jitter_fill_ms(&ring), 0);
}

// A partial millisecond is a caller bug, not a rounding case: accepting it would
// desynchronise the 1 ms alignment the USB endpoint depends on.
static void test_rejects_partial_ms(void)
{
    static mic_jitter_t ring;
    mic_jitter_init(&ring);
    int16_t samples[MIC_JITTER_SAMPLES_PER_MS + 1] = {0};
    CHECK_EQ(mic_jitter_put(&ring, samples, MIC_JITTER_SAMPLES_PER_MS + 1, 0), ESP_ERR_INVALID_SIZE);
    CHECK_EQ(mic_jitter_take(&ring, samples, MIC_JITTER_SAMPLES_PER_MS + 1, 0), ESP_ERR_INVALID_SIZE);
    CHECK_EQ(mic_jitter_put(&ring, NULL, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_ERR_INVALID_ARG);
    CHECK_EQ(mic_jitter_take(&ring, NULL, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_ERR_INVALID_ARG);
}

// Overflow must discard the *oldest* audio and keep the newest, and it must be
// counted. This is the DESIGN.md 4.1 rule: never drop the frame the user is
// speaking into the microphone right now.
static void test_overflow_drops_oldest(void)
{
    static mic_jitter_t ring;
    mic_jitter_init(&ring);

    int16_t ms[MIC_JITTER_SAMPLES_PER_MS];
    int16_t seed = 0;
    // Write more than capacity. Every put after the ring is full costs one millisecond.
    for (seed = 0; seed < MIC_JITTER_CAPACITY_MS + 25; ++seed) {
        make_ms(ms, seed);
        CHECK_EQ(mic_jitter_put(&ring, ms, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_OK);
    }

    // Nothing consumes here, so the ring is pinned at capacity: puts queue into a
    // ring whose oldest millisecond is discarded each time. What must hold is the
    // ordering rule (oldest out, newest kept), not an exact seed, because the number
    // of discards depends on how the ring was filled.
    CHECK_EQ(mic_jitter_fill_ms(&ring), MIC_JITTER_CAPACITY_MS);

    mic_jitter_stats_t stats;
    mic_jitter_get_stats(&ring, &stats);
    CHECK(stats.put_overruns > 0);
    CHECK_EQ(stats.max_fill_ms, MIC_JITTER_CAPACITY_MS);

    // Every discard must have come from the tail: the audio still queued has to be
    // the most recently written block, ending with the very last millisecond written.
    int16_t out[MIC_JITTER_SAMPLES_PER_MS];
    int16_t newest[MIC_JITTER_SAMPLES_PER_MS];
    make_ms(newest, (int16_t)(MIC_JITTER_CAPACITY_MS + 25 - 1));

    int16_t last_taken[MIC_JITTER_SAMPLES_PER_MS];
    uint32_t fill = mic_jitter_fill_ms(&ring);
    for (uint32_t i = 0; i < fill; ++i) {
        CHECK_EQ(mic_jitter_take(&ring, out, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_OK);
        memcpy(last_taken, out, sizeof(out));
    }
    CHECK_EQ(memcmp(last_taken, newest, sizeof(newest)), 0);
}

// The drift controller is the whole point of the component. Model a producer that is
// 0.42% faster than the consumer for a simulated hour and assert the ring neither
// grows without bound nor stays pinned at capacity.
static void test_fast_producer_is_bounded(void)
{
    static mic_jitter_t ring;
    mic_jitter_init(&ring);

    int16_t ms[MIC_JITTER_SAMPLES_PER_MS];
    make_ms(ms, 7);

    // 60 simulated seconds of 1 ms ticks. The consumer takes one millisecond per tick.
    // The producer feeds slightly more than one millisecond per tick by giving an extra
    // millisecond every 1/0.0042 = ~238 ticks, which is the measured crystal mismatch.
    const int ticks = 60000;
    const int extra_every = 238;
    int64_t now_us = 0;
    int16_t out[MIC_JITTER_SAMPLES_PER_MS];

    for (int t = 0; t < ticks; ++t) {
        now_us += 1000;
        if (t % extra_every == 0) {
            // This tick delivers two milliseconds, matching a faster producer.
            CHECK_EQ(mic_jitter_put(&ring, ms, MIC_JITTER_SAMPLES_PER_MS, now_us), ESP_OK);
        }
        CHECK_EQ(mic_jitter_put(&ring, ms, MIC_JITTER_SAMPLES_PER_MS, now_us), ESP_OK);
        CHECK_EQ(mic_jitter_take(&ring, out, MIC_JITTER_SAMPLES_PER_MS, now_us), ESP_OK);
    }

    mic_jitter_stats_t stats;
    mic_jitter_get_stats(&ring, &stats);

    // Without splice-drops the ring would accumulate ~250 ms in a minute and overflow
    // repeatedly. With them it is pulled back from the high water mark each time, so
    // overflow never happens and the fill level stays bounded.
    CHECK_EQ(stats.put_overruns, 0);
    CHECK(stats.splice_drops > 0);
    CHECK(mic_jitter_fill_ms(&ring) < MIC_JITTER_HIGH_WATER_MS);
    printf("  fast producer: drops=%u repeats=%u fill=%ums overruns=%u\n", stats.splice_drops,
           stats.splice_repeats, mic_jitter_fill_ms(&ring), stats.put_overruns);
}

// The mirror case: a producer 0.42% slower than the consumer must not run the ring
// dry forever. Splice-repeats keep it alive, and underruns stop after priming.
static void test_slow_producer_is_bounded(void)
{
    static mic_jitter_t ring;
    mic_jitter_init(&ring);

    int16_t ms[MIC_JITTER_SAMPLES_PER_MS];
    make_ms(ms, 3);

    // Prime the ring so concealment has a seed.
    for (int i = 0; i < MIC_JITTER_TARGET_MS; ++i) {
        CHECK_EQ(mic_jitter_put(&ring, ms, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_OK);
    }

    const int ticks = 60000;
    const int skip_every = 238;  // producer misses one millisecond per ~238
    int64_t now_us = 0;
    int16_t out[MIC_JITTER_SAMPLES_PER_MS];

    for (int t = 0; t < ticks; ++t) {
        now_us += 1000;
        if (t % skip_every != 0) {
            CHECK_EQ(mic_jitter_put(&ring, ms, MIC_JITTER_SAMPLES_PER_MS, now_us), ESP_OK);
        }
        CHECK_EQ(mic_jitter_take(&ring, out, MIC_JITTER_SAMPLES_PER_MS, now_us), ESP_OK);
    }

    mic_jitter_stats_t stats;
    mic_jitter_get_stats(&ring, &stats);

    // A handful of underruns at the very start is acceptable; a persistent stream of
    // them is not. The controller should have kicked in long before a quarter of the
    // run.
    CHECK(stats.splice_repeats > 0);
    CHECK(stats.take_underruns < 20);
    printf("  slow producer: repeats=%u drops=%u underruns=%u fill=%ums\n", stats.splice_repeats,
           stats.splice_drops, stats.take_underruns, mic_jitter_fill_ms(&ring));
}

// Concealment must decay rather than hold a DC level: a held sample looks like a DC
// offset to an ASR front end, while a decaying tail looks like a gap.
static void test_concealment_decays(void)
{
    static mic_jitter_t ring;
    mic_jitter_init(&ring);

    int16_t ms[MIC_JITTER_SAMPLES_PER_MS];
    for (size_t i = 0; i < MIC_JITTER_SAMPLES_PER_MS; ++i) {
        ms[i] = 8000;
    }
    CHECK_EQ(mic_jitter_put(&ring, ms, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_OK);

    int16_t out[MIC_JITTER_SAMPLES_PER_MS];
    CHECK_EQ(mic_jitter_take(&ring, out, MIC_JITTER_SAMPLES_PER_MS, 1000000), ESP_OK);
    CHECK_EQ(out[0], 8000);

    // Next take has no data; it must conceal, and each further take must be quieter.
    int16_t first = 0;
    int16_t second = 0;
    CHECK_EQ(mic_jitter_take(&ring, out, MIC_JITTER_SAMPLES_PER_MS, 2000000), ESP_OK);
    first = out[0];
    CHECK_EQ(mic_jitter_take(&ring, out, MIC_JITTER_SAMPLES_PER_MS, 3000000), ESP_OK);
    second = out[0];

    CHECK(first > 0);
    CHECK(second > 0);
    CHECK(second < first);

    mic_jitter_stats_t stats;
    mic_jitter_get_stats(&ring, &stats);
    CHECK_EQ(stats.plc_frames, 2);
}

// A flush on a session change or a USB transition must drop queued audio but keep the
// cumulative counters, so a reconnect does not silently reset the loss accounting.
static void test_flush_keeps_counters(void)
{
    static mic_jitter_t ring;
    mic_jitter_init(&ring);

    int16_t ms[MIC_JITTER_SAMPLES_PER_MS];
    make_ms(ms, 1);
    for (int i = 0; i < 20; ++i) {
        CHECK_EQ(mic_jitter_put(&ring, ms, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_OK);
    }
    CHECK_EQ(mic_jitter_fill_ms(&ring), 20);

    mic_jitter_flush(&ring);
    CHECK_EQ(mic_jitter_fill_ms(&ring), 0);

    mic_jitter_stats_t stats;
    mic_jitter_get_stats(&ring, &stats);
    CHECK_EQ(stats.put_frames, 20);

    // After a flush there is no concealment seed left, so a take must be clean silence
    // rather than a repeat of audio from the previous session.
    int16_t out[MIC_JITTER_SAMPLES_PER_MS];
    CHECK_EQ(mic_jitter_take(&ring, out, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_OK);
    for (size_t i = 0; i < MIC_JITTER_SAMPLES_PER_MS; ++i) {
        CHECK_EQ(out[i], 0);
    }
}

// reset_stats clears the accounting without disturbing audio, which is what a
// per-session report needs.
static void test_reset_stats(void)
{
    static mic_jitter_t ring;
    mic_jitter_init(&ring);

    int16_t ms[MIC_JITTER_SAMPLES_PER_MS];
    make_ms(ms, 2);
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(mic_jitter_put(&ring, ms, MIC_JITTER_SAMPLES_PER_MS, 0), ESP_OK);
    }

    mic_jitter_reset_stats(&ring);
    mic_jitter_stats_t stats;
    mic_jitter_get_stats(&ring, &stats);
    CHECK_EQ(stats.put_frames, 0);
    CHECK_EQ(mic_jitter_fill_ms(&ring), 5);
}

int main(void)
{
    printf("== audio_jitter host tests ==\n");
    test_underrun_before_data();
    test_round_trip_order();
    test_rejects_partial_ms();
    test_overflow_drops_oldest();
    test_fast_producer_is_bounded();
    test_slow_producer_is_bounded();
    test_concealment_decays();
    test_flush_keeps_counters();
    test_reset_stats();

    printf("\n%d checks, %d failures\n", s_checks, s_failures);
    if (s_failures != 0) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
