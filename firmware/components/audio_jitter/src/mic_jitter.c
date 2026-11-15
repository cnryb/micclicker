// Receiver-side PCM ring with adaptive clock-drift compensation.
//
// See mic_jitter.h for the contract. The interesting parts here are the two
// controllers: the overflow guard on the producer side and the drift splice on the
// consumer side. They are deliberately separate, because they respond to different
// problems. An overflow means the consumer stopped draining (USB host paused, alt
// setting off); a drift splice means the two clocks disagree by a fraction of a
// percent and the ring is slowly walking away from its target.

#include "mic_jitter.h"

#include <string.h>

#include "freertos/FreeRTOS.h"  // shimmed on the host, real in firmware
#include "freertos/semphr.h"

#define MIC_JITTER_CAPACITY_SAMPLES ((size_t)MIC_JITTER_CAPACITY_MS * MIC_JITTER_SAMPLES_PER_MS)

static uint32_t samples_to_ms(size_t count)
{
    return (uint32_t)(count / MIC_JITTER_SAMPLES_PER_MS);
}

void mic_jitter_init(mic_jitter_t *ring)
{
    if (ring == NULL) {
        return;
    }
    memset(ring, 0, sizeof(*ring));
    if (ring->lock == NULL) {
        ring->lock = xSemaphoreCreateMutex();
    }
    ring->capacity_ms = MIC_JITTER_CAPACITY_MS;
    ring->target_ms = MIC_JITTER_TARGET_MS;
    ring->high_water_ms = MIC_JITTER_HIGH_WATER_MS;
    ring->low_water_ms = MIC_JITTER_LOW_WATER_MS;
    // A splice "never happened" until one does, so the first correction is not
    // suppressed by the spacing rule.
    ring->last_splice_us = 0;
    ring->frozen = false;
    ring->stats.min_fill_ms = UINT32_MAX;
}

void mic_jitter_flush(mic_jitter_t *ring)
{
    if (ring == NULL) {
        return;
    }
    if (ring->lock == NULL || xSemaphoreTake(ring->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    ring->head = 0;
    ring->tail = 0;
    ring->count = 0;
    ring->have_last_ms = false;
    ring->primed = false;
    memset(ring->last_ms, 0, sizeof(ring->last_ms));
    xSemaphoreGive(ring->lock);
}

esp_err_t mic_jitter_put(mic_jitter_t *ring, const int16_t *samples, size_t count, int64_t now_us)
{
    (void)now_us;
    if (ring == NULL || samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count == 0) {
        return ESP_OK;
    }
    if (count % MIC_JITTER_SAMPLES_PER_MS != 0) {
        // Partial milliseconds would desynchronise the 1 ms alignment both ends rely
        // on, so they are rejected rather than rounded.
        return ESP_ERR_INVALID_SIZE;
    }
    size_t capacity = MIC_JITTER_CAPACITY_SAMPLES;
    if (count > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t dropped_ms = 0;

    if (ring->lock == NULL || xSemaphoreTake(ring->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    while (ring->count + count > capacity) {
        // Discard the oldest millisecond, never the frame that just arrived: the
        // newest audio is what the user is speaking right now.
        ring->tail = (ring->tail + MIC_JITTER_SAMPLES_PER_MS) % (uint32_t)capacity;
        ring->count -= MIC_JITTER_SAMPLES_PER_MS;
        ++dropped_ms;
    }

    for (size_t i = 0; i < count; ++i) {
        ring->buffer[(ring->head + i) % (uint32_t)capacity] = samples[i];
    }
    ring->head = (ring->head + (uint32_t)count) % (uint32_t)capacity;
    ring->count += (uint32_t)count;

    // The last accepted millisecond is the concealment seed for a future gap.
    memcpy(ring->last_ms, &samples[count - MIC_JITTER_SAMPLES_PER_MS], sizeof(ring->last_ms));
    ring->have_last_ms = true;

    uint32_t fill_ms = samples_to_ms(ring->count);
    if (!ring->primed && fill_ms >= ring->target_ms) {
        // The ring has now held a full target. From here on a low fill is drift or a
        // link gap rather than startup, so the low-water correction is allowed.
        ring->primed = true;
    }
    if (fill_ms > ring->stats.max_fill_ms) {
        ring->stats.max_fill_ms = fill_ms;
    }
    ++ring->stats.put_frames;
    ring->stats.put_overruns += dropped_ms;

    xSemaphoreGive(ring->lock);
    return ESP_OK;
}

// Conceal `count` samples by repeating the last accepted millisecond with a slow
// decay, so a short gap fades instead of switching to digital silence. A full hard
// zero is reserved for the case where nothing has ever been received.
static void conceal(mic_jitter_t *ring, int16_t *out, size_t count)
{
    if (!ring->have_last_ms) {
        memset(out, 0, count * sizeof(int16_t));
        ring->stats.silence_frames += samples_to_ms(count) == 0 ? 1 : samples_to_ms(count);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        int16_t source = ring->last_ms[i % MIC_JITTER_SAMPLES_PER_MS];
        // 7/8 decay per repeated millisecond: audible as a brief tail, gone quickly
        // enough that a long gap does not turn into a tone.
        int32_t faded = (int32_t)source * 7 / 8;
        out[i] = (int16_t)faded;
    }
    // Decay the seed so consecutive concealed milliseconds keep fading.
    for (size_t i = 0; i < MIC_JITTER_SAMPLES_PER_MS; ++i) {
        ring->last_ms[i] = (int16_t)((int32_t)ring->last_ms[i] * 7 / 8);
    }
    ring->stats.plc_frames += samples_to_ms(count) == 0 ? 1 : samples_to_ms(count);
}

esp_err_t mic_jitter_take(mic_jitter_t *ring, int16_t *samples, size_t count, int64_t now_us)
{
    if (ring == NULL || samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count == 0) {
        return ESP_OK;
    }
    if (count % MIC_JITTER_SAMPLES_PER_MS != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t capacity = MIC_JITTER_CAPACITY_SAMPLES;

    if (ring->lock == NULL || xSemaphoreTake(ring->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t fill_ms = samples_to_ms(ring->count);

    if (!ring->frozen) {
        // Correct only at the emergency thresholds, and only once per gate interval.
        // The time gate is what bounds the correction rate: without it a large fill
        // error would produce a burst of splices instead of a slow convergence.
        bool past_gate = ring->last_splice_us == 0 ||
                         (now_us - ring->last_splice_us) >= MIC_JITTER_SPLICE_MIN_INTERVAL_US;
        if (past_gate && fill_ms >= ring->high_water_ms) {
            // Nearing overflow: the producer is faster than the consumer. Skipping a
            // millisecond of queued audio pulls the fill level back down.
            if (ring->count >= MIC_JITTER_SAMPLES_PER_MS) {
                ring->tail = (ring->tail + MIC_JITTER_SAMPLES_PER_MS) % (uint32_t)capacity;
                ring->count -= MIC_JITTER_SAMPLES_PER_MS;
                ring->last_splice_us = now_us;
                ++ring->stats.splice_drops;
                fill_ms = samples_to_ms(ring->count);
            }
        } else if (past_gate && ring->primed && fill_ms <= ring->low_water_ms &&
                   ring->have_last_ms) {
            // Nearing dry-out while already streaming. Repeat the most recent queued
            // millisecond, which is real audio from this session. The
            // `primed` condition keeps this from firing during startup: a ring that
            // has never reached its target is still filling, and duplicating audio
            // there would corrupt the first frames of every session.
            if (ring->count >= MIC_JITTER_SAMPLES_PER_MS &&
                ring->count + MIC_JITTER_SAMPLES_PER_MS <= capacity) {
                size_t last = (ring->tail + ring->count - MIC_JITTER_SAMPLES_PER_MS) % capacity;
                for (size_t i = 0; i < MIC_JITTER_SAMPLES_PER_MS; ++i) {
                    ring->buffer[(ring->head + i) % capacity] = ring->buffer[(last + i) % capacity];
                }
                ring->head = (ring->head + MIC_JITTER_SAMPLES_PER_MS) % (uint32_t)capacity;
                ring->count += MIC_JITTER_SAMPLES_PER_MS;
                ring->last_splice_us = now_us;
                ++ring->stats.splice_repeats;
                fill_ms = samples_to_ms(ring->count);
            }
        }
    }

    if (ring->count >= count) {
        for (size_t i = 0; i < count; ++i) {
            samples[i] = ring->buffer[(ring->tail + i) % (uint32_t)capacity];
        }
        ring->tail = (ring->tail + (uint32_t)count) % (uint32_t)capacity;
        ring->count -= (uint32_t)count;
        ++ring->stats.take_frames;
    } else {
        // Not enough queued: serve what is there, conceal the rest. A USB isochronous
        // endpoint must always deliver a full packet, so this never short-reads.
        size_t available = ring->count;
        for (size_t i = 0; i < available; ++i) {
            samples[i] = ring->buffer[(ring->tail + i) % (uint32_t)capacity];
        }
        ring->tail = (ring->tail + (uint32_t)available) % (uint32_t)capacity;
        ring->count = 0;
        conceal(ring, &samples[available], count - available);
        ++ring->stats.take_frames;
        ++ring->stats.take_underruns;
    }

    uint32_t after_ms = samples_to_ms(ring->count);
    if (after_ms < ring->stats.min_fill_ms) {
        ring->stats.min_fill_ms = after_ms;
    }

    xSemaphoreGive(ring->lock);
    return ESP_OK;
}

uint32_t mic_jitter_fill_ms(const mic_jitter_t *ring)
{
    if (ring == NULL) {
        return 0;
    }
    // Reading without the lock can tear on a 32-bit target only if `count` changes
    // while it is read; a single aligned word load is atomic on both cores here, and
    // this value is a status sample rather than a control input.
    return samples_to_ms(ring->count);
}

void mic_jitter_get_stats(const mic_jitter_t *ring, mic_jitter_stats_t *out)
{
    if (ring == NULL || out == NULL) {
        return;
    }
    if (ring->lock == NULL || xSemaphoreTake(ring->lock, portMAX_DELAY) != pdTRUE) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = ring->stats;
    xSemaphoreGive(ring->lock);
    if (out->min_fill_ms == UINT32_MAX) {
        out->min_fill_ms = 0;
    }
}

void mic_jitter_reset_stats(mic_jitter_t *ring)
{
    if (ring == NULL) {
        return;
    }
    if (ring->lock == NULL || xSemaphoreTake(ring->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    memset(&ring->stats, 0, sizeof(ring->stats));
    ring->stats.min_fill_ms = UINT32_MAX;
    xSemaphoreGive(ring->lock);
}
