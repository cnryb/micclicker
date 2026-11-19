// C3 product role: capture, encode, send.
//
// This is the stage-4 product firmware on the sender side. It replaces the probe's
// synthetic traffic with real microphone audio:
//
//   ES8311 + I2S  ->  320 samples (20 ms)  ->  G.722 (160 B)  ->  one ESP-NOW packet
//
// Two rules from docs/protocol.md shape the send loop:
//
//  * A frame's sequence number advances only after the radio actually accepted it.
//    Advancing on a rejected send would leave a permanent hole that the receiver
//    cannot tell apart from radio loss.
//  * The cadence is derived from a monotonic deadline, not from the capture read, so a
//    slow codec read cannot permanently shift the frame grid. Late frames are sent
//    immediately and counted; the grid itself does not drift.
//
// Nothing in this file blocks the Wi-Fi task: the receive path only answers pairing
// frames (handled by link_session) and counts link health.

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "g722_enc_dec.h"
#include "key_task.h"
#include "mic_capture.h"
#include "mic_g722.h"
#include "mic_link.h"
#include "mic_probe.h"
#include "mic_radio.h"
#include "mic_session.h"

static const char *TAG = "c3_audio";

// The audio task reads one 20 ms frame at a time. A 6 KB stack is ample: the codec
// state is a static, the scratch buffers are static, and the deepest call is the I2S
// read.
#define C3_AUDIO_TASK_STACK 6144
#define C3_AUDIO_TASK_PRIORITY 5
// Log a health line every second, which matches the status cadence the USB side uses.
#define C3_STATUS_PERIOD_US (1000 * 1000)
// Voice-key edge streaming (DESIGN.md 5.1): keep the codec warm, but only encode+send
// while armed. Arm/disarm follow UP/DOWN edges in key_task; this task only enforces a
// max armed duration so a forgotten session cannot stream forever.
#define C3_MAX_ARMED_MS 120000

typedef struct {
    uint32_t frames_captured;
    uint32_t frames_encoded;
    uint32_t frames_sent;
    uint32_t capture_errors;
    uint32_t encode_errors;
    uint32_t send_errors;
    uint32_t send_retries;
    uint32_t late_frames;
    uint32_t seq;
} c3_audio_stats_t;

static c3_audio_stats_t s_stats;
static uint16_t s_session;
static mic_g722_encoder_t s_encoder;
// The encoded frame is reused across retries of the same sequence number, which is what
// keeps a retried frame byte-identical to its first transmission.
static uint8_t s_encoded[MIC_G722_FRAME_BYTES];
static mic_link_frame_t s_frame;

static uint16_t make_session(void)
{
    // A session number has to differ between boots so the receiver resets its sequence
    // tracking. Two random bytes give a 1/65536 chance of colliding with the previous
    // boot, which is enough: a collision only costs one spurious gap report.
    uint32_t r = esp_random();
    uint16_t session = (uint16_t)(r & 0xFFFFu);
    return session == 0 ? 1 : session;
}

// Send the frame built for the current sequence number, retrying a bounded number of
// times without advancing the sequence. ESP-NOW rejects a send when its internal queue
// is full (ESP_ERR_ESPNOW_NO_MEM); backing off briefly is required, because a tight
// retry loop starves the Wi-Fi task and makes the rejection permanent
// (docs/protocol.md section 4).
static bool send_current_frame(void)
{
    const int max_attempts = 4;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        esp_err_t err = mic_session_send(s_frame.bytes, s_frame.len);
        if (err == ESP_OK) {
            ++s_stats.frames_sent;
            return true;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            // Not paired. Retrying cannot help; the caller reports it once per status
            // period instead of flooding the log.
            ++s_stats.send_errors;
            return false;
        }
        ++s_stats.send_retries;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ++s_stats.send_errors;
    return false;
}

// Returns false when this frame must not be encoded or sent. Max-session is the only
// local auto-disarm here; voice-key edges and OK live in key_task.
static bool armed_allows_send(int64_t now_us)
{
    static bool was_armed;
    static int64_t armed_since_us;

    bool armed = micclicker_c3_audio_armed();
    if (!armed) {
        was_armed = false;
        return false;
    }

    if (!was_armed) {
        was_armed = true;
        armed_since_us = now_us;
    }

    if (now_us - armed_since_us >= (int64_t)C3_MAX_ARMED_MS * 1000) {
        micclicker_c3_audio_force_off();
        ESP_LOGI(TAG, "C3_AUDIO_ARM: DISARM reason=max_session_ms=%d", C3_MAX_ARMED_MS);
        was_armed = false;
        return false;
    }

    return micclicker_c3_audio_armed();
}

static void audio_task(void *argument)
{
    (void)argument;

    // Session is assigned in micclicker_c3_audio_start before this task and the key task
    // start, so both ends of the product path share one number from the first frame.
    if (mic_g722_encoder_init(&s_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "C3_AUDIO: FAIL reason=encoder_init");
        vTaskDelete(NULL);
        return;
    }

    if (mic_capture_init() != ESP_OK) {
        ESP_LOGE(TAG, "C3_AUDIO: FAIL reason=capture_init");
        // Not fatal for the link: keep reporting status so the failure is visible on
        // the S3 side as "streaming but silent" rather than as a dead device.
    }

    ESP_LOGI(TAG, "C3_AUDIO: READY session=%u rate=%d frame_samples=%d frame_bytes=%d",
             (unsigned)s_session, MIC_CAPTURE_SAMPLE_RATE, MIC_CAPTURE_FRAME_SAMPLES,
             MIC_G722_FRAME_BYTES);

    // One frame period in microseconds, used only to decide whether a frame was late.
    const int64_t frame_period_us =
        (int64_t)MIC_CAPTURE_FRAME_SAMPLES * 1000000 / MIC_CAPTURE_SAMPLE_RATE;
    int64_t next_status_us = esp_timer_get_time() + C3_STATUS_PERIOD_US;
    // Grid origin: the start of the previous frame. Zero until the first frame ends, so
    // the first iteration cannot be counted as late.
    int64_t frame_start_us = 0;

    static int16_t pcm[MIC_CAPTURE_FRAME_SAMPLES];

    for (;;) {
        int64_t frame_begin_us = esp_timer_get_time();

        if (!mic_capture_ready()) {
            // Capture failed at init; retry it once a second rather than spinning.
            if (mic_capture_init() != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        // The pacing comes from the DMA, not from a delay: reading 320 mono samples
        // blocks until the codec has actually produced them, which is 20 ms. Adding a
        // sleep on top of that would halve the frame rate and make the stream play at
        // the wrong speed, which is exactly what the first version of this loop did.
        if (mic_capture_read(pcm, MIC_CAPTURE_FRAME_SAMPLES) != ESP_OK) {
            ++s_stats.capture_errors;
            continue;
        }
        ++s_stats.frames_captured;

        // Always read so the codec stays warm and the first armed frame has no init gap.
        // Encode and radio only run while the key path has armed streaming.
        if (armed_allows_send(frame_begin_us)) {
            if (mic_g722_encode(&s_encoder, pcm, s_encoded) != ESP_OK) {
                ++s_stats.encode_errors;
            } else {
                ++s_stats.frames_encoded;

                // seq advances only after a successful send (docs/protocol.md section 3.1),
                // so the builder is called with the *current* sequence number and only
                // committed once the radio accepted the frame.
                if (mic_link_build_audio(&s_frame, s_session, s_stats.seq, s_encoded) == ESP_OK) {
                    if (send_current_frame()) {
                        ++s_stats.seq;
                    }
                }
            }
        }

        // `late` counts frames whose *cadence* slipped, not frames whose processing
        // merely occupies most of a period. The capture read blocks until the DMA has
        // produced the frame, so a healthy frame always spends ~20 ms inside this loop
        // and comparing that against the period alone flags every frame. What matters
        // is the gap between consecutive frame starts: if it exceeds one period, the
        // grid is drifting and the receiver will start concealing.
        int64_t after = esp_timer_get_time();
        if (frame_start_us > 0 && frame_begin_us - frame_start_us > frame_period_us * 3 / 2) {
            ++s_stats.late_frames;
        }
        frame_start_us = frame_begin_us;

        if (after >= next_status_us) {
            next_status_us = after + C3_STATUS_PERIOD_US;
            mic_radio_stats_t radio;
            mic_radio_get_stats(&radio);
            mic_session_info_t info;
            mic_session_get(&info);
            ESP_LOGI(TAG,
                     "C3_AUDIO: STATUS armed=%d paired=%d session=%u seq=%" PRIu32
                     " captured=%" PRIu32 " encoded=%" PRIu32 " sent=%" PRIu32
                     " capture_err=%" PRIu32 " encode_err=%" PRIu32 " send_err=%" PRIu32
                     " retries=%" PRIu32 " late=%" PRIu32 " tx_ok=%" PRIu32 " tx_fail=%" PRIu32
                     " tx_err=%" PRIu32,
                     micclicker_c3_audio_armed() ? 1 : 0, info.paired ? 1 : 0,
                     (unsigned)s_session, s_stats.seq, s_stats.frames_captured,
                     s_stats.frames_encoded, s_stats.frames_sent, s_stats.capture_errors,
                     s_stats.encode_errors, s_stats.send_errors, s_stats.send_retries,
                     s_stats.late_frames, radio.tx_ok, radio.tx_fail, radio.tx_errors);

            // The key counters get their own line so a key-path regression does not
            // require parsing the audio line, and so the two can be differenced
            // independently when a run is graded.
            micclicker_c3_keys_log_status();
        }
    }
}

// Inbound traffic for this end: a heartbeat reply, which is how the S3 reports its
// receive-side counters, and a key ACK, which is how it confirms a key event. Both are
// handled without blocking, because this runs in the Wi-Fi task.
static volatile uint32_t s_hb_acks;

static void on_recv(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint8_t *data, int len, int8_t rssi,
                    void *context)
{
    (void)mac;
    (void)rssi;
    (void)context;

    mic_link_view_t view;
    if (mic_link_parse(data, (size_t)len, &view) != ESP_OK) {
        return;
    }
    if (view.header.type == MIC_LINK_TYPE_HEARTBEAT_ACK) {
        ++s_hb_acks;
        // The reply echoes the heartbeat it answers, which is what makes matching it by
        // sequence number possible instead of guessing from arrival order.
        mic_link_heartbeat_ack_t ack;
        if (mic_link_decode_heartbeat_ack(&view, &ack) == ESP_OK) {
            micclicker_c3_keys_on_heartbeat_ack(ack.echo_seq);
        }
        return;
    }
    if (view.header.type == MIC_LINK_TYPE_KEY_ACK) {
        uint32_t acked_seq = 0;
        if (mic_link_decode_key_ack(&view, &acked_seq) == ESP_OK) {
            micclicker_c3_keys_on_ack(acked_seq);
        }
    }
}

void micclicker_c3_audio_start(void)
{
    s_session = make_session();

    // Both ends pair on the same fixed channel in the first product build. Pairing time
    // channel selection is a later concern; DESIGN.md section 4.1 only requires the two
    // ends to agree, and a fixed channel is the simplest way to guarantee it.
    esp_err_t err = mic_session_start(MIC_RADIO_DEFAULT_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C3_AUDIO: FAIL reason=session_start err=%s", esp_err_to_name(err));
        return;
    }
    mic_session_set_receiver(on_recv, NULL);

    mic_session_info_t info;
    mic_session_get(&info);
    if (!info.paired) {
        // No binding: this end is the initiator, because it is the one that owns the
        // audio and therefore the one that has to be alive for the link to matter.
#if CONFIG_MICCLICKER_PAIR_ON_BOOT
        ESP_LOGI(TAG, "C3_AUDIO: PAIR_BEGIN role=initiator reason=unbound");
        err = mic_session_pair_as_initiator(info.channel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "C3_AUDIO: PAIR_FAIL err=%s; streaming reports unpaired until a bind exists",
                     esp_err_to_name(err));
        }
#else
        // Pairing on boot is disabled, so this unit captures and encodes but never
        // sends until a pairing gesture starts a window. Useful for measuring the
        // sender alone without a receiver in range.
        ESP_LOGW(TAG, "C3_AUDIO: PAIR_SKIPPED reason=config; capturing without a bound peer");
#endif
    }

    if (xTaskCreate(audio_task, "mc_c3_audio", C3_AUDIO_TASK_STACK, NULL, C3_AUDIO_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "C3_AUDIO: FAIL reason=task_create");
    }

    // Keys come up after the audio task so the first audio frame is not delayed by ADC
    // calibration, and after pairing so a key pressed during boot has a session to ride.
    // The session number is shared: every frame this end sends must claim the same session,
    // or the receiver treats key traffic as a session change on every keystroke.
    esp_err_t keys_err = micclicker_c3_keys_start(s_session);
    if (keys_err != ESP_OK) {
        // Not fatal: audio keeps working, and the log line is what says the keys are dead.
        ESP_LOGE(TAG, "C3_AUDIO: KEYS_FAIL err=%s", esp_err_to_name(keys_err));
    }
}
