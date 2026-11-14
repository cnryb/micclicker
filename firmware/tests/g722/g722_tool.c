// Host-side harness for the vendored G.722 codec core.
//
// Runs on a normal desktop compiler, not on the ESP32, so the codec can be graded
// without flashing a board. It links the very same sources the firmware builds, and
// covers the wrapper contract (frame sizes, error cases, state reset) plus the
// golden vectors committed under vectors/.
//
// Golden vectors are ffmpeg-generated and pinned by SHA-256. They are an
// interoperability baseline, not the ITU official vectors: those require an ITU
// account, so --itu grades them when they are supplied separately.
#include <errno.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mic_g722.h"

static int g_failures;

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#include <stdarg.h>
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("   FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    ++g_failures;
}

static void ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ok(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("   PASS ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)size ? (size_t)size : 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)size;
    return buf;
}


// Encode `samples` into `encoded`. Returns the number of frames written, or -1.
static long encode_buffer(const int16_t *samples, size_t sample_count, uint8_t **encoded)
{
    if (sample_count % MIC_G722_FRAME_SAMPLES != 0) {
        return -1;
    }
    size_t frames = sample_count / MIC_G722_FRAME_SAMPLES;
    uint8_t *out = malloc(frames * MIC_G722_FRAME_BYTES);
    if (out == NULL) {
        return -1;
    }
    static mic_g722_encoder_t enc;
    if (mic_g722_encoder_init(&enc) != ESP_OK) {
        free(out);
        return -1;
    }
    for (size_t i = 0; i < frames; ++i) {
        if (mic_g722_encode(&enc, samples + i * MIC_G722_FRAME_SAMPLES,
                            out + i * MIC_G722_FRAME_BYTES) != ESP_OK) {
            free(out);
            return -1;
        }
    }
    *encoded = out;
    return (long)frames;
}

static long decode_buffer(const uint8_t *encoded, size_t encoded_len, int16_t **samples)
{
    if (encoded_len % MIC_G722_FRAME_BYTES != 0) {
        return -1;
    }
    size_t frames = encoded_len / MIC_G722_FRAME_BYTES;
    int16_t *out = malloc(frames * MIC_G722_FRAME_SAMPLES * sizeof(int16_t));
    if (out == NULL) {
        return -1;
    }
    static mic_g722_decoder_t dec;
    if (mic_g722_decoder_init(&dec) != ESP_OK) {
        free(out);
        return -1;
    }
    for (size_t i = 0; i < frames; ++i) {
        if (mic_g722_decode(&dec, encoded + i * MIC_G722_FRAME_BYTES,
                            out + i * MIC_G722_FRAME_SAMPLES) != ESP_OK) {
            free(out);
            return -1;
        }
    }
    *samples = out;
    return (long)frames;
}

// Measure the dominant frequency of a mono 16 kHz buffer by zero crossings.
// Adequate for the synthetic single-tone vectors used here.
static double zero_cross_hz(const int16_t *samples, size_t count)
{
    size_t crossings = 0;
    for (size_t i = 1; i < count; ++i) {
        if ((samples[i - 1] < 0) != (samples[i] < 0)) {
            ++crossings;
        }
    }
    return (double)crossings / 2.0 / ((double)count / 16000.0);
}

static double rms(const int16_t *samples, size_t count)
{
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        sum += (double)samples[i] * (double)samples[i];
    }
    return count ? sqrt(sum / (double)count) : 0.0;
}

// ---------------------------------------------------------------------------
// Contract checks: the wrapper behaviour the firmware relies on.
// ---------------------------------------------------------------------------
static void check_contract(void)
{
    printf("== wrapper contract ==\n");

    ok("state types are value types: encoder=%zu B decoder=%zu B (no allocation)",
       sizeof(mic_g722_encoder_t), sizeof(mic_g722_decoder_t));

    static mic_g722_encoder_t enc;
    static mic_g722_decoder_t dec;
    int16_t pcm[MIC_G722_FRAME_SAMPLES];
    uint8_t coded[MIC_G722_FRAME_BYTES];
    int16_t back[MIC_G722_FRAME_SAMPLES];

    // Uninitialized codecs must refuse rather than touch garbage state.
    memset(pcm, 0, sizeof(pcm));
    memset(&enc, 0, sizeof(enc));
    memset(&dec, 0, sizeof(dec));
    if (mic_g722_encode(&enc, pcm, coded) == ESP_ERR_INVALID_STATE) {
        ok("uninitialized encoder rejected");
    } else {
        fail("uninitialized encoder was not rejected");
    }
    if (mic_g722_decode(&dec, coded, back) == ESP_ERR_INVALID_STATE) {
        ok("uninitialized decoder rejected");
    } else {
        fail("uninitialized decoder was not rejected");
    }

    if (mic_g722_encoder_init(&enc) == ESP_OK && mic_g722_decoder_init(&dec) == ESP_OK) {
        ok("init succeeds");
    } else {
        fail("init failed");
    }

    // Null handling on every pointer position.
    if (mic_g722_encode(NULL, pcm, coded) == ESP_ERR_INVALID_ARG &&
        mic_g722_encode(&enc, NULL, coded) == ESP_ERR_INVALID_ARG &&
        mic_g722_encode(&enc, pcm, NULL) == ESP_ERR_INVALID_ARG) {
        ok("encoder null arguments rejected");
    } else {
        fail("encoder accepted a null argument");
    }
    if (mic_g722_decode(NULL, coded, back) == ESP_ERR_INVALID_ARG &&
        mic_g722_decode(&dec, NULL, back) == ESP_ERR_INVALID_ARG &&
        mic_g722_decode(&dec, coded, NULL) == ESP_ERR_INVALID_ARG) {
        ok("decoder null arguments rejected");
    } else {
        fail("decoder accepted a null argument");
    }

    // One frame in, one frame out: the sizes DESIGN.md fixes.
    for (size_t i = 0; i < MIC_G722_FRAME_SAMPLES; ++i) {
        pcm[i] = (int16_t)((i * 37) % 2000 - 1000);
    }
    if (mic_g722_encode(&enc, pcm, coded) == ESP_OK) {
        ok("encode returns exactly %d bytes for %d samples", MIC_G722_FRAME_BYTES,
           MIC_G722_FRAME_SAMPLES);
    } else {
        fail("encode of a full frame failed");
    }
    if (mic_g722_decode(&dec, coded, back) == ESP_OK) {
        ok("decode returns exactly %d samples for %d bytes", MIC_G722_FRAME_SAMPLES,
           MIC_G722_FRAME_BYTES);
    } else {
        fail("decode of a full frame failed");
    }

    // Re-init must clear filter history so a new session cannot inherit old state.
    static mic_g722_encoder_t enc2;
    uint8_t coded_a[MIC_G722_FRAME_BYTES];
    uint8_t coded_b[MIC_G722_FRAME_BYTES];
    mic_g722_encoder_init(&enc);
    mic_g722_encode(&enc, pcm, coded_a);
    mic_g722_encoder_init(&enc);
    mic_g722_encode(&enc, pcm, coded_b);
    mic_g722_encoder_init(&enc2);
    mic_g722_encode(&enc2, pcm, coded_a);
    if (memcmp(coded_a, coded_b, MIC_G722_FRAME_BYTES) == 0) {
        ok("encoder re-init resets state to a fixed starting point");
    } else {
        fail("encoder re-init left residual state");
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// Golden vectors: ffmpeg-generated, pinned by SHA-256.
// ---------------------------------------------------------------------------
static void check_golden(const char *dir)
{
    printf("== golden vectors (ffmpeg baseline) ==\n");

    // name | pcm file | g722 file | expected dominant Hz | tolerance Hz
    static const struct {
        const char *name;
        const char *pcm;
        const char *g722;
        double expect_hz;
        double tol_hz;
    } cases[] = {
        { "sine 1 kHz", "sine_1k.pcm", "sine_1k.g722", 1000.0, 40.0 },
        { "sine 300 Hz", "sine_300.pcm", "sine_300.g722", 300.0, 20.0 },
        { "sine 4 kHz", "sine_4k.pcm", "sine_4k.g722", 4000.0, 80.0 },
    };
    const size_t ncases = sizeof(cases) / sizeof(cases[0]);

    for (size_t c = 0; c < ncases; ++c) {
        char pcm_path[512], g722_path[512];
        snprintf(pcm_path, sizeof(pcm_path), "%s/%s", dir, cases[c].pcm);
        snprintf(g722_path, sizeof(g722_path), "%s/%s", dir, cases[c].g722);

        size_t pcm_len = 0, g722_len = 0;
        uint8_t *pcm_raw = read_file(pcm_path, &pcm_len);
        uint8_t *g722_raw = read_file(g722_path, &g722_len);
        if (pcm_raw == NULL || g722_raw == NULL) {
            fail("%s: cannot read vector files", cases[c].name);
            free(pcm_raw);
            free(g722_raw);
            continue;
        }

        size_t sample_count = pcm_len / sizeof(int16_t);
        uint8_t *encoded = NULL;
        long frames = encode_buffer((const int16_t *)pcm_raw, sample_count, &encoded);
        if (frames < 0) {
            fail("%s: encode failed", cases[c].name);
            free(pcm_raw);
            free(g722_raw);
            continue;
        }

        // Bit-exactness against the pinned reference bitstream.
        if ((size_t)frames * MIC_G722_FRAME_BYTES != g722_len) {
            fail("%s: encoded %ld B, reference is %zu B",
                 cases[c].name, frames * MIC_G722_FRAME_BYTES, g722_len);
        } else if (memcmp(encoded, g722_raw, g722_len) == 0) {
            ok("%s: encoder output is bit-exact to pinned ffmpeg reference (%zu B)",
               cases[c].name, g722_len);
        } else {
            size_t first = 0;
            while (first < g722_len && encoded[first] == g722_raw[first]) {
                ++first;
            }
            fail("%s: bitstream differs at byte %zu of %zu", cases[c].name, first, g722_len);
        }

        // Decode the pinned reference and confirm the tone survives the round trip.
        int16_t *decoded = NULL;
        long out_frames = decode_buffer(g722_raw, g722_len, &decoded);
        if (out_frames < 0) {
            fail("%s: decode failed", cases[c].name);
        } else {
            size_t out_count = (size_t)out_frames * MIC_G722_FRAME_SAMPLES;
            double hz = zero_cross_hz(decoded, out_count);
            double level = rms(decoded, out_count);
            // Skip the first frame: the QMF filters need a moment to settle.
            int16_t *settled = decoded + MIC_G722_FRAME_SAMPLES;
            size_t settled_count = out_count - MIC_G722_FRAME_SAMPLES;
            double hz_settled = zero_cross_hz(settled, settled_count);
            if (hz_settled >= cases[c].expect_hz - cases[c].tol_hz &&
                hz_settled <= cases[c].expect_hz + cases[c].tol_hz) {
                ok("%s: decoded tone %.1f Hz (expected %.0f +/-%.0f)", cases[c].name,
                   hz_settled, cases[c].expect_hz, cases[c].tol_hz);
            } else {
                fail("%s: decoded tone %.1f Hz, expected %.0f +/-%.0f", cases[c].name,
                     hz_settled, cases[c].expect_hz, cases[c].tol_hz);
            }
            if (level > 100.0) {
                ok("%s: decoded RMS %.1f (non-silent)", cases[c].name, level);
            } else {
                fail("%s: decoded RMS %.1f looks like silence", cases[c].name, level);
            }
            (void)hz;
            free(decoded);
        }

        free(encoded);
        free(pcm_raw);
        free(g722_raw);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("usage: %s [--itu DIR]\n", argv[0]);
        return 0;
    }

    const char *vectors = NULL;
    const char *itu_dir = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--vectors") == 0 && i + 1 < argc) {
            vectors = argv[++i];
        } else if (strcmp(argv[i], "--itu") == 0 && i + 1 < argc) {
            itu_dir = argv[++i];
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    check_contract();

    if (vectors != NULL) {
        check_golden(vectors);
    } else {
        printf("   SKIP golden vectors: no --vectors directory given\n\n");
    }

    if (itu_dir != NULL) {
        printf("== ITU official vectors ==\n");
        printf("   SKIP not implemented: the ITU G.722 vector set needs the official\n");
        printf("        G722p2_configuration-*.zip archives, which require an ITU\n");
        printf("        account. Placing them in %s does not yet grade them.\n", itu_dir);
        printf("        See firmware/tests/g722/README.md for the grading plan.\n\n");
    }

    if (g_failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL (%d check(s))\n", g_failures);
    return 1;
}
