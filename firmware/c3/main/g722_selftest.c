#include "micclicker_g722_selftest.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mic_g722.h"

static const char *TAG = "g722_selftest";

// Frame 0 of firmware/tests/g722/vectors/sine_1k.pcm: a 1 kHz tone at amplitude
// 12000, 320 samples. The matching expected bitstream below is the same frame
// encoded by ffmpeg, so matching it on-device proves the chip builds a codec that
// interoperates with an independent implementation, not just with itself.
static const int16_t VECTOR_PCM[MIC_G722_FRAME_SAMPLES] = {
    0, 4592, 8485, 11087, 12000, 11087, 8485, 4592, 0, -4592, -8485, -11087,
    -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
    0, -4592, -8485, -11087, -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087,
    12000, 11087, 8485, 4592, 0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
    0, 4592, 8485, 11087, 12000, 11087, 8485, 4592, 0, -4592, -8485, -11087,
    -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
    0, -4592, -8485, -11087, -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087,
    12000, 11087, 8485, 4592, 0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
    0, 4592, 8485, 11087, 12000, 11087, 8485, 4592, 0, -4592, -8485, -11087,
    -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
    0, -4592, -8485, -11087, -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087,
    12000, 11087, 8485, 4592, 0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
    0, 4592, 8485, 11087, 12000, 11087, 8485, 4592, 0, -4592, -8485, -11087,
    -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
    0, -4592, -8485, -11087, -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087,
    12000, 11087, 8485, 4592, 0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
    0, 4592, 8485, 11087, 12000, 11087, 8485, 4592, 0, -4592, -8485, -11087,
    -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
    0, -4592, -8485, -11087, -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087,
    12000, 11087, 8485, 4592, 0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
    0, 4592, 8485, 11087, 12000, 11087, 8485, 4592, 0, -4592, -8485, -11087,
    -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
    0, -4592, -8485, -11087, -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087,
    12000, 11087, 8485, 4592, 0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
    0, 4592, 8485, 11087, 12000, 11087, 8485, 4592, 0, -4592, -8485, -11087,
    -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
    0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
};

static const uint8_t VECTOR_G722[MIC_G722_FRAME_BYTES] = {
    0xfa, 0x90, 0x22, 0x8b, 0x24, 0x8c, 0xa0, 0xa0, 0x20, 0xab, 0xc6, 0xca, 0xcc, 0xdc, 0xeb, 0xe7,
    0xea, 0x79, 0xd0, 0xcc, 0xce, 0xde, 0xac, 0xe8, 0xeb, 0xfc, 0x50, 0xcd, 0x90, 0xdf, 0xed, 0xaa,
    0xed, 0xfc, 0xd2, 0x4e, 0xd1, 0x9f, 0xef, 0xeb, 0xef, 0xbd, 0xd4, 0x4f, 0xd3, 0xbf, 0xf0, 0xec,
    0xef, 0xff, 0xd3, 0xd1, 0x94, 0xdf, 0xf0, 0xec, 0x70, 0xfd, 0x94, 0xd0, 0x94, 0xfe, 0xf1, 0xad,
    0xf1, 0x7d, 0xd6, 0x92, 0xd6, 0xff, 0xb2, 0xef, 0x73, 0xff, 0xd7, 0x54, 0x96, 0xbd, 0xf2, 0xee,
    0x73, 0xff, 0xd7, 0xd5, 0x97, 0xfd, 0xf2, 0xf0, 0xf1, 0xff, 0xd6, 0xd4, 0x95, 0xfd, 0xf3, 0xb0,
    0xf1, 0xff, 0xd6, 0xd4, 0x95, 0xfd, 0xf3, 0xb0, 0xf1, 0xfd, 0xd7, 0xd4, 0xd5, 0xff, 0xb3, 0xf0,
    0xf1, 0xfd, 0xd8, 0xd2, 0x98, 0xdf, 0xf1, 0xb0, 0x71, 0xbd, 0xd7, 0xd5, 0x94, 0xde, 0xf3, 0xf1,
    0xf1, 0xfd, 0xd9, 0xd3, 0x97, 0xfe, 0xf8, 0xef, 0xf5, 0xfa, 0xd6, 0xd4, 0x94, 0xdd, 0xf3, 0xf2,
    0x71, 0xba, 0x98, 0xd4, 0x95, 0xfd, 0xb3, 0xf1, 0x6f, 0xbd, 0x5e, 0xd5, 0xd6, 0xbb, 0xf6, 0xed,
};

bool micclicker_g722_selftest_run(void)
{
    static mic_g722_encoder_t encoder;
    static mic_g722_decoder_t decoder;
    uint8_t encoded[MIC_G722_FRAME_BYTES];
    int16_t decoded[MIC_G722_FRAME_SAMPLES];
    bool ok = true;

    if (mic_g722_encoder_init(&encoder) != ESP_OK || mic_g722_decoder_init(&decoder) != ESP_OK) {
        ESP_LOGE(TAG, "G722_SELFTEST: FAIL reason=init");
        return false;
    }

    // Encode the committed reference frame and compare with the ffmpeg bitstream.
    // This is the check that would catch a wrong bitrate/option combination or a
    // toolchain-dependent arithmetic difference.
    if (mic_g722_encode(&encoder, VECTOR_PCM, encoded) != ESP_OK) {
        ESP_LOGE(TAG, "G722_SELFTEST: FAIL reason=encode");
        return false;
    }
    if (memcmp(encoded, VECTOR_G722, MIC_G722_FRAME_BYTES) != 0) {
        size_t first = 0;
        while (first < MIC_G722_FRAME_BYTES && encoded[first] == VECTOR_G722[first]) {
            ++first;
        }
        ESP_LOGE(TAG, "G722_SELFTEST: FAIL reason=bitstream_mismatch byte=%zu got=0x%02x want=0x%02x",
                 first, encoded[first], VECTOR_G722[first]);
        ok = false;
    } else {
        ESP_LOGI(TAG, "G722_SELFTEST: ENCODE_BITEXACT bytes=%d", MIC_G722_FRAME_BYTES);
    }

    // Decode the reference bitstream and confirm the tone and level survive. A
    // bit-exact encoder with a broken decoder would otherwise still look healthy.
    if (mic_g722_decode(&decoder, VECTOR_G722, decoded) != ESP_OK) {
        ESP_LOGE(TAG, "G722_SELFTEST: FAIL reason=decode");
        return false;
    }
    int32_t peak = 0;
    int64_t energy = 0;
    for (size_t i = 0; i < MIC_G722_FRAME_SAMPLES; ++i) {
        int32_t v = decoded[i];
        if (v < 0) {
            v = -v;
        }
        if (v > peak) {
            peak = v;
        }
        energy += (int64_t)decoded[i] * decoded[i];
    }
    // Silence or a saturated mess both indicate a decode failure; the reference
    // decodes to roughly half scale.
    if (peak < 4000 || peak > 20000) {
        ESP_LOGE(TAG, "G722_SELFTEST: FAIL reason=decoded_peak peak=%ld", (long)peak);
        ok = false;
    } else {
        ESP_LOGI(TAG, "G722_SELFTEST: DECODE_OK peak=%ld", (long)peak);
    }

    // Sustained encode cost matters more than a single frame on the audio path, so
    // time 100 frames and report microseconds per frame against the 20 ms budget.
    static mic_g722_encoder_t timing;
    mic_g722_encoder_init(&timing);
    const int frames = 100;
    int64_t start = esp_timer_get_time();
    for (int i = 0; i < frames; ++i) {
        mic_g722_encode(&timing, VECTOR_PCM, encoded);
    }
    int64_t elapsed = esp_timer_get_time() - start;
    int64_t per_frame_us = elapsed / frames;
    // 20 ms per frame is the hard deadline; anything close to it leaves no headroom
    // for the rest of the audio path, so flag it well before the limit.
    if (per_frame_us > 10000) {
        ESP_LOGE(TAG, "G722_SELFTEST: FAIL reason=encode_too_slow us_per_frame=%ld",
                 (long)per_frame_us);
        ok = false;
    } else {
        ESP_LOGI(TAG, "G722_SELFTEST: TIMING frames=%d us_per_frame=%ld budget_us=20000",
                 frames, (long)per_frame_us);
    }

    if (ok) {
        ESP_LOGI(TAG, "G722_SELFTEST: PASS bitexact=1 decode=1 us_per_frame=%ld",
                 (long)per_frame_us);
    }
    return ok;
}
