#pragma once

// Receiver-side PCM ring with adaptive clock-drift compensation.
//
// The two ends of the MicClicker link do not share a clock. C3 samples at its own
// 16 kHz crystal and S3 drains the ring at the USB host's 16 kHz, and hardware-c3.md
// records a measured ~0.42% difference between such crystals. A fixed buffer
// therefore fills or empties on a schedule of minutes, which is exactly what a
// 30-minute recording cannot tolerate.
//
// This component absorbs that drift by making the ring *elastic*: the producer and
// the consumer each see an interface that hides the other, and the ring itself
// decides, from its own fill level, when to skip or duplicate one millisecond of
// audio. Because a drop or a repeat happens at most once per drift period, the
// audible effect is a single 1 ms splice rather than a periodic dropout.
//
// Everything here is 1 kHz-oriented: the caller works in microseconds and 16-bit
// mono samples, and the ring converts to whole sample offsets itself.
//
// Threading: put and take each tolerate exactly one caller task, and they may run on
// different cores. The head/tail/count state is guarded by a FreeRTOS recursiveless
// mutex, not a portMUX spinlock: both callers are tasks that may block briefly, and a
// spinlock taken on one core while the other holds it longer than the interrupt
// watchdog allows trips the watchdog in the Wi-Fi task context. This component is
// therefore not callable from an ISR.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// 16 kHz * 1 ms. The ring works in whole milliseconds because that is the unit the
// USB isochronous endpoint transfers, and a splice at that granularity is inaudible.
#define MIC_JITTER_SAMPLES_PER_MS 16

// Ring capacity in milliseconds. 200 ms is four times the target fill level, so a
// normal Wi-Fi hiccup of a few tens of milliseconds never overflows, while a stalled
// producer still surfaces as silence quickly enough to be obvious.
#define MIC_JITTER_CAPACITY_MS 200

// Fill level the ring is normally steered toward, in milliseconds. It has to sit
// comfortably above the observed ESP-NOW burst latency (tens of ms) and well below
// capacity, so normal jitter is absorbed without any correction at all.
#define MIC_JITTER_TARGET_MS 60

// The controller corrects only outside these two emergency thresholds, not merely
// outside a band around the target. That distinction matters: a ring that is below
// target because it just started, or because a burst of frames arrived late, is
// working correctly and must not be "corrected" by duplicating audio.
//
//   high water: close enough to capacity that an overflow is imminent
//   low water:  close enough to empty that a dry-out is imminent
//
// With the measured ~0.42% crystal mismatch the fill level walks at roughly 4 ms/s,
// so a producer that starts at the target crosses the high water mark after some
// seconds and is pulled back with a single 1 ms splice. Correcting rarely is what
// makes the correction inaudible.
#define MIC_JITTER_HIGH_WATER_MS (MIC_JITTER_CAPACITY_MS - 40)
#define MIC_JITTER_LOW_WATER_MS  12

// Minimum spacing between two splices. At the measured 0.42% drift a splice is needed
// every ~240 ms, so 150 ms is well inside that and still prevents a burst of
// corrections from a single large fill error.
#define MIC_JITTER_SPLICE_MIN_INTERVAL_US 150000

// Diagnostics. Every counter is cumulative for the lifetime of the ring; the caller
// reports the ones that matter for the acceptance baseline and resets on a new
// session if it wants per-session figures.
typedef struct {
    uint32_t put_frames;        // successful mic_jitter_put calls
    uint32_t put_overruns;      // put calls that dropped the oldest millisecond
    uint32_t take_frames;       // successful mic_jitter_take calls
    uint32_t take_underruns;    // take calls served with silence because it was dry
    uint32_t splice_drops;      // milliseconds discarded to reduce fill
    uint32_t splice_repeats;    // milliseconds repeated to increase fill
    uint32_t plc_frames;        // concealed frames synthesized for a link gap
    uint32_t silence_frames;    // frames filled with silence (underrun or mute)
    uint32_t max_fill_ms;       // high-water mark of the fill level
    uint32_t min_fill_ms;       // low-water mark, reported as a large value when unset
} mic_jitter_stats_t;

typedef struct {
    // Guards head/tail/fill accounting across the producer and consumer tasks. Created
    // by mic_jitter_init; NULL until then, which is why every entry point checks it.
    SemaphoreHandle_t lock;
    // Index of the next slot to write, and of the next slot to read, in samples.
    uint32_t head;
    uint32_t tail;
    // Samples currently queued, maintained explicitly instead of derived from
    // head/tail so a full and an empty ring stay distinguishable.
    uint32_t count;
    uint32_t capacity_ms;
    uint32_t target_ms;
    uint32_t high_water_ms;
    uint32_t low_water_ms;
    int64_t last_splice_us;
    // One millisecond of G.722-domain concealment state: the last accepted samples,
    // repeated with a gentle decay instead of a hard zero, so a short gap sounds like
    // a click rather than a drop-out.
    int16_t last_ms[MIC_JITTER_SAMPLES_PER_MS];
    bool have_last_ms;
    // Set once the ring has held a full target worth of audio. Until then the ring is
    // priming: low fill is the expected startup condition, not drift, and duplicating
    // audio to "fix" it would corrupt the first frames of every session.
    bool primed;
    // When set, the controller never splices and never conceals: every take that has
    // no data yields silence. Used before the first frame ever arrives.
    bool frozen;
    mic_jitter_stats_t stats;
    int16_t buffer[MIC_JITTER_CAPACITY_MS * MIC_JITTER_SAMPLES_PER_MS];
} mic_jitter_t;

// Initialize `ring` with the compile-time capacity and target. Safe to call on a
// zeroed struct; it also resets every counter.
void mic_jitter_init(mic_jitter_t *ring);

// Drop everything queued and reset the per-session concealment state, while keeping
// the cumulative counters intact. Called on a session change, a link timeout, or a
// USB alternative-setting transition, so stale audio cannot leak across a boundary.
void mic_jitter_flush(mic_jitter_t *ring);

// Queue `samples` (must be a multiple of MIC_JITTER_SAMPLES_PER_MS). When the ring
// cannot accept the whole block it drops the oldest queued milliseconds to make room,
// which is the "discard the oldest audio" rule from DESIGN.md section 4.1.
esp_err_t mic_jitter_put(mic_jitter_t *ring, const int16_t *samples, size_t count,
                         int64_t now_us);

// Dequeue `samples`. On underrun the requested samples are filled with concealment
// (decayed repeat of the last accepted millisecond) and the call still returns
// ESP_OK, because a short read is worse than a small artifact for a UAC endpoint
// that must deliver a fixed packet every millisecond.
esp_err_t mic_jitter_take(mic_jitter_t *ring, int16_t *samples, size_t count, int64_t now_us);

// Fill level in milliseconds, rounded down. Cheap enough to sample once per second
// for the status report.
uint32_t mic_jitter_fill_ms(const mic_jitter_t *ring);

// Take a consistent copy of the counters. The caller must not read `ring->stats`
// directly: the producer and consumer both bump it.
void mic_jitter_get_stats(const mic_jitter_t *ring, mic_jitter_stats_t *out);

// Reset the counters without touching queued audio.
void mic_jitter_reset_stats(mic_jitter_t *ring);

#ifdef __cplusplus
}
#endif
