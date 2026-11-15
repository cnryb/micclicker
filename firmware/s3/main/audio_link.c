// S3 product role: receive, decode, buffer, output.
//
// This is the stage-4 product firmware on the receiver side. It replaces the stage-2
// prototype's hard-coded 1 kHz sine table with real audio from the link:
//
//   one ESP-NOW packet  ->  G.722 decode  ->  320 samples  ->  ring  ->  UAC2 endpoint
//
// The receive callback runs in the Wi-Fi task, so it does the minimum: validate the
// frame, decode it when it is the next expected one, and push it into the ring. Anything
// that could block (logging, key reporting, status) is left to the product task.
//
// Sequence handling follows docs/protocol.md: audio is never retransmitted, a gap is
// concealed rather than waited on, and effective loss is holes minus late arrivals.

#include "mic_link_audio.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_g722.h"
#include "mic_jitter.h"
#include "mic_link.h"
#include "mic_radio.h"
#include "mic_session.h"

static const char *TAG = "s3_audio";

// A heartbeat every second is what lets the C3 side confirm the link is alive and what
// gives the S3 side a place to report its counters. It matches the status cadence.
#define S3_STATUS_PERIOD_US (1000 * 1000)
#define S3_RX_TASK_STACK 4096
#define S3_RX_TASK_PRIORITY 6

typedef struct {
    uint32_t audio_rx;
    uint32_t audio_dup;
    uint32_t audio_gap;
    uint32_t audio_late;
    uint32_t audio_decoded;
    uint32_t audio_decode_errors;
    uint32_t rejected;
    uint32_t heartbeats;
    uint32_t heartbeat_replies;
} s3_rx_stats_t;

static s3_rx_stats_t s_stats;
static mic_g722_decoder_t s_decoder;
static mic_jitter_t s_ring;
static mic_link_session_t s_sessions;
// The ring is written by the Wi-Fi task and read by the USB side, so the helper that
// exposes it is the only handoff between the two.
static int64_t s_last_audio_us;

// Heartbeat replies have their own sequence counter (docs/protocol.md section 3.2), and
// only the product task writes it, so no lock is needed for it.
static uint32_t s_reply_seq;

mic_jitter_t *mic_link_audio_ring(void)
{
    return &s_ring;
}

void mic_link_audio_get_stats(mic_link_audio_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->audio_rx = s_stats.audio_rx;
    out->audio_dup = s_stats.audio_dup;
    out->audio_gap = s_sessions.audio.holes;
    out->audio_late = s_sessions.audio.late;
    out->audio_decoded = s_stats.audio_decoded;
    out->audio_decode_errors = s_stats.audio_decode_errors;
    out->rejected = s_stats.rejected;
    out->heartbeats = s_stats.heartbeats;
}

// Whether the host is actually consuming the endpoint. The ring must not be fed while
// the endpoint is idle: nobody drains it, so every arriving frame would evict the
// oldest one and the counters would fill with meaningless overflow before the host ever
// opens the microphone. Set by the USB side through mic_link_audio_set_consuming().
static volatile bool s_consuming;

void mic_link_audio_set_consuming(bool consuming)
{
    if (s_consuming != consuming) {
        s_consuming = consuming;
        // A transition in either direction is a session boundary for the ring: stale
        // audio must not be played when the host starts listening, and there is no point
        // accumulating it while the host is not.
        mic_jitter_flush(&s_ring);
        ESP_LOGI(TAG, "S3_AUDIO: CONSUMING state=%d", consuming ? 1 : 0);
    }
}

// The receive callback. Wi-Fi task context: no logging, no NVS, no allocation.
static void on_audio_frame(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint8_t *data, int len,
                           int8_t rssi, void *context)
{
    (void)mac;
    (void)rssi;
    (void)context;

    mic_link_view_t view;
    if (mic_link_parse(data, (size_t)len, &view) != ESP_OK) {
        ++s_stats.rejected;
        return;
    }

    if (view.header.type == MIC_LINK_TYPE_HEARTBEAT) {
        ++s_stats.heartbeats;
        return;  // answered by the product task, which may log
    }

    if (view.header.type != MIC_LINK_TYPE_AUDIO) {
        // Keys, ACKs and pairing frames are not part of the stage-4 audio chain. They
        // are counted as rejected here so the audio counters stay honest, and stage 5
        // will give them their own path.
        ++s_stats.rejected;
        return;
    }

    // A new session restarts the sender's numbering, so the trackers and the ring both
    // have to be reset or the first frames of the new session look like a huge gap.
    if (mic_link_session_adopt(&s_sessions, view.header.session)) {
        s_stats.audio_rx = 0;
        s_stats.audio_dup = 0;
        s_stats.audio_decode_errors = 0;
        mic_jitter_flush(&s_ring);
        // The decoder holds filter state from the previous session. Resetting it keeps
        // the first frame from inheriting stale history.
        mic_g722_decoder_init(&s_decoder);
        ESP_EARLY_LOGI(TAG, "S3_AUDIO: SESSION_NEW session=%u", (unsigned)view.header.session);
    }

    if (mic_link_seq_is_duplicate(&s_sessions.audio, view.header.seq)) {
        ++s_stats.audio_dup;
        mic_link_seq_observe(&s_sessions.audio, view.header.seq);
        return;
    }
    mic_link_seq_observe(&s_sessions.audio, view.header.seq);
    ++s_stats.audio_rx;

    const uint8_t *payload = NULL;
    if (mic_link_decode_audio(&view, &payload) != ESP_OK) {
        ++s_stats.rejected;
        return;
    }

    static int16_t pcm[MIC_G722_FRAME_SAMPLES];
    if (mic_g722_decode(&s_decoder, payload, pcm) != ESP_OK) {
        ++s_stats.audio_decode_errors;
        return;
    }
    ++s_stats.audio_decoded;

    // Frames are decoded even while the host is not listening, so the decode counters
    // and the loss accounting stay meaningful, but only queued when the endpoint is
    // actually being drained. Queuing into an idle ring would just evict older frames
    // and report overflow that no listener ever experienced.
    if (s_consuming) {
        mic_jitter_put(&s_ring, pcm, MIC_G722_FRAME_SAMPLES, esp_timer_get_time());
    }
    s_last_audio_us = esp_timer_get_time();
}

// Answer a heartbeat with the receiver's cumulative counters, which is what the sender
// diffs into per-window figures (docs/protocol.md section 3.4).
static void send_heartbeat_reply(const mic_link_heartbeat_t *beat, uint32_t echo_seq)
{
    mic_link_heartbeat_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.echo_seq = echo_seq;
    ack.phase = beat->phase;
    ack.state = mic_session_ready() ? MIC_LINK_STATE_PAIRED : 0;
    ack.flags = 0;
    ack.audio_rx = s_stats.audio_rx;
    ack.audio_dup = s_stats.audio_dup;
    ack.audio_gap = s_sessions.audio.holes;
    ack.audio_late = s_sessions.audio.late;
    ack.key_rx = 0;
    ack.key_dup = 0;
    ack.rejected = s_stats.rejected;

    mic_link_frame_t frame;
    if (mic_link_build_heartbeat_ack(&frame, 0, ++s_reply_seq, &ack) != ESP_OK) {
        return;
    }
    if (mic_session_send(frame.bytes, frame.len) == ESP_OK) {
        ++s_stats.heartbeat_replies;
    }
}

// The product task owns everything the Wi-Fi task must not do: logging, status
// reporting, and answering heartbeats (which sends, and sending from the receive
// callback would re-enter ESP-NOW).
//
// Heartbeat frames are counted in the callback and answered here from a snapshot, so a
// missed heartbeat costs one log line rather than a radio deadlock.
static volatile bool s_heartbeat_pending;

static void status_task(void *argument)
{
    (void)argument;

    int64_t next_status_us = esp_timer_get_time() + S3_STATUS_PERIOD_US;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));

        int64_t now = esp_timer_get_time();

        if (s_heartbeat_pending) {
            s_heartbeat_pending = false;
            mic_link_heartbeat_t beat = {.phase = 0, .mode = MIC_LINK_HEARTBEAT_MODE_PROBE, .flags = 0};
            send_heartbeat_reply(&beat, 0);
        }

        if (now >= next_status_us) {
            next_status_us = now + S3_STATUS_PERIOD_US;

            mic_jitter_stats_t jitter;
            mic_jitter_get_stats(&s_ring, &jitter);
            mic_radio_stats_t radio;
            mic_radio_get_stats(&radio);
            mic_session_info_t info;
            mic_session_get(&info);

            uint32_t effective_loss =
                s_sessions.audio.holes > s_sessions.audio.late
                    ? s_sessions.audio.holes - s_sessions.audio.late
                    : 0;

            ESP_LOGI(TAG,
                     "S3_AUDIO: STATUS paired=%d fill_ms=%u rx=%" PRIu32 " dup=%" PRIu32
                     " decoded=%" PRIu32 " gap=%" PRIu32 " late=%" PRIu32 " loss=%" PRIu32
                     " decode_err=%" PRIu32 " rejected=%" PRIu32 " underruns=%" PRIu32
                     " overruns=%" PRIu32 " plc=%" PRIu32 " drops=%" PRIu32 " repeats=%" PRIu32
                     " max_fill=%u min_fill=%u tx_ok=%" PRIu32 " rx_frames=%" PRIu32,
                     info.paired ? 1 : 0, mic_jitter_fill_ms(&s_ring), s_stats.audio_rx,
                     s_stats.audio_dup, s_stats.audio_decoded, s_sessions.audio.holes,
                     s_sessions.audio.late, effective_loss, s_stats.audio_decode_errors,
                     s_stats.rejected, jitter.take_underruns, jitter.put_overruns, jitter.plc_frames,
                     jitter.splice_drops, jitter.splice_repeats, jitter.max_fill_ms,
                     jitter.min_fill_ms, radio.tx_ok, radio.rx_frames);

            if (now - s_last_audio_us > 3000000 && s_stats.audio_rx > 0) {
                ESP_LOGW(TAG, "S3_AUDIO: SILENT_3S no audio for over 3 s; check the C3 sender");
            }
        }
    }
}

void micclicker_s3_audio_start(void)
{
    mic_jitter_init(&s_ring);
    mic_link_session_reset(&s_sessions);
    if (mic_g722_decoder_init(&s_decoder) != ESP_OK) {
        ESP_LOGE(TAG, "S3_AUDIO: FAIL reason=decoder_init");
        return;
    }

    esp_err_t err = mic_session_start(MIC_RADIO_DEFAULT_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "S3_AUDIO: FAIL reason=session_start err=%s", esp_err_to_name(err));
        return;
    }
    mic_session_set_receiver(on_audio_frame, NULL);

    mic_session_info_t info;
    mic_session_get(&info);
    if (!info.paired) {
        // The C3 owns the keys because it is the end that can be reset independently
        // while the USB side stays attached. This end therefore waits to be paired.
        // An unbound receiver waits indefinitely. A bounded window only works when the
        // two boards happen to be reset in the right order, and this end has nothing
        // else to do until a peer appears: USB stays enumerated and the endpoint
        // outputs silence meanwhile.
        ESP_LOGI(TAG, "S3_AUDIO: PAIR_WAIT role=responder reason=unbound window=forever");
        err = mic_session_pair_as_responder(MIC_SESSION_PAIR_FOREVER);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "S3_AUDIO: PAIR_WAIT result=none err=%s; USB stays enumerated and the "
                          "endpoint outputs silence until a bind exists",
                     esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "S3_AUDIO: READY ring_ms=%d target_ms=%d high_water=%d", MIC_JITTER_CAPACITY_MS,
             MIC_JITTER_TARGET_MS, MIC_JITTER_HIGH_WATER_MS);

    if (xTaskCreate(status_task, "mc_s3_status", S3_RX_TASK_STACK, NULL, S3_RX_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "S3_AUDIO: FAIL reason=task_create");
    }
}
