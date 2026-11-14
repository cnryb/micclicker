// ESP-NOW probe, receiver role (S3).
//
// Accepts the sender's traffic, validates every frame with the shared codec, verifies
// each audio payload against its sequence number, answers heartbeats with a cumulative
// counter snapshot, and performs the receiver side of pairing. It logs one summary per
// phase tag so its numbers can be diffed against the sender's.
//
// The receive callback runs in the Wi-Fi task: it validates, copies at most one small
// event into a ring, and returns. Anything that could block (logging a summary,
// writing NVS) happens on the probe task.

#include "mic_probe.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_bind.h"
#include "mic_link.h"
#include "mic_probe_shared.h"
#include "mic_radio.h"

static const char *TAG = "probe_rx";

#define PROBE_RX_RING 64
#define PROBE_LOG_PERIOD_MS 1000
// The receiver has to outlive the sender's whole schedule, which is roughly 8 s start
// delay plus discovery, burst, paced, key and pairing phases. 150 s leaves room for a
// slow discovery and for the host to attach a console after the reset.
#define PROBE_PAIR_WINDOW_MS 150000

// The receiver learns the sender's address from the first accepted frame, then adds it
// as a unicast peer for the heartbeat replies.
static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static volatile bool s_peer_known;
static volatile uint16_t s_peer_session;
static volatile bool s_session_adopted;

static mic_link_session_t s_sessions;

// Counters the receiver reports in each heartbeat reply. All cumulative for the
// session, which is what lets the sender diff two snapshots into per-phase figures.
static volatile uint32_t s_audio_rx;
static volatile uint32_t s_audio_dup;
static volatile uint32_t s_audio_gap;
static volatile uint32_t s_audio_late;
static volatile uint32_t s_audio_payload_errors;
static volatile uint32_t s_key_rx;
static volatile uint32_t s_key_dup;
static volatile uint32_t s_rejected;
static volatile uint32_t s_hb_rx;

// Events the callback hands to the probe task.
typedef enum {
    PROBE_EVENT_HEARTBEAT = 0,
    PROBE_EVENT_PAIR_REQ,
} probe_event_kind_t;

typedef struct {
    probe_event_kind_t kind;
    uint8_t mac[ESP_NOW_ETH_ALEN];
    mic_link_header_t header;
    mic_link_heartbeat_t heartbeat;
    mic_link_pair_req_t pair;
} probe_event_t;

static probe_event_t s_ring[PROBE_RX_RING];
static volatile uint32_t s_ring_head;
static volatile uint32_t s_ring_tail;
static volatile uint32_t s_ring_overflow;

static bool s_encrypted_peer;
static uint8_t s_peer_lmk[ESP_NOW_KEY_LEN];
static uint8_t s_reply_seq;

static void on_recv(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint8_t *data, int len, int8_t rssi,
                    void *context)
{
    (void)rssi;
    (void)context;

    mic_link_view_t view;
    if (mic_link_parse(data, (size_t)len, &view) != ESP_OK) {
        ++s_rejected;
        return;
    }

    // Pairing frames are exempt from session tracking: they arrive before the sender
    // has announced a session and before either end has accepted the other as a peer.
    if (view.header.type == MIC_LINK_TYPE_PAIR_REQ) {
        mic_link_pair_req_t request;
        if (mic_link_decode_pair_req(&view, &request) != ESP_OK) {
            ++s_rejected;
            return;
        }
        uint32_t head = s_ring_head;
        uint32_t next = (head + 1) % PROBE_RX_RING;
        if (next == s_ring_tail) {
            ++s_ring_overflow;
            return;
        }
        s_ring[head].kind = PROBE_EVENT_PAIR_REQ;
        memcpy(s_ring[head].mac, mac, ESP_NOW_ETH_ALEN);
        s_ring[head].header = view.header;
        s_ring[head].pair = request;
        s_ring_head = next;
        return;
    }

    if (!s_session_adopted || view.header.session != s_peer_session) {
        mic_link_session_adopt(&s_sessions, view.header.session);
        s_peer_session = view.header.session;
        s_session_adopted = true;
        // A new session restarts the sender's numbering and invalidates its keys, so
        // the receiver's per-session counters restart with it.
        s_audio_rx = 0;
        s_audio_dup = 0;
        s_audio_gap = 0;
        s_audio_late = 0;
        s_audio_payload_errors = 0;
        s_key_rx = 0;
        s_key_dup = 0;
        ESP_LOGI(TAG, "PROBE_SESSION session=%u changes=%" PRIu32, (unsigned)view.header.session,
                 s_sessions.session_changes);
    }

    if (!s_peer_known) {
        memcpy(s_peer_mac, mac, ESP_NOW_ETH_ALEN);
        s_peer_known = true;
    }

    switch (view.header.type) {
    case MIC_LINK_TYPE_AUDIO: {
        const uint8_t *payload = NULL;
        if (mic_link_decode_audio(&view, &payload) != ESP_OK) {
            ++s_rejected;
            return;
        }
        if (mic_link_seq_is_duplicate(&s_sessions.audio, view.header.seq)) {
            ++s_audio_dup;
            mic_link_seq_observe(&s_sessions.audio, view.header.seq);
            return;
        }
        mic_link_seq_observe(&s_sessions.audio, view.header.seq);
        ++s_audio_rx;
        // Payload verification happens on the probe task for accepted frames only, so a
        // duplicate or a hole never disturbs the count.
        if (probe_check_audio(payload, view.header.seq) != 0) {
            ++s_audio_payload_errors;
        }
        return;
    }
    case MIC_LINK_TYPE_KEY: {
        mic_link_key_t key;
        if (mic_link_decode_key(&view, &key) != ESP_OK) {
            ++s_rejected;
            return;
        }
        if (mic_link_seq_is_duplicate(&s_sessions.key, view.header.seq)) {
            ++s_key_dup;
            mic_link_seq_observe(&s_sessions.key, view.header.seq);
            return;
        }
        mic_link_seq_observe(&s_sessions.key, view.header.seq);
        ++s_key_rx;
        return;
    }
    case MIC_LINK_TYPE_HEARTBEAT: {
        mic_link_heartbeat_t beat;
        if (mic_link_decode_heartbeat(&view, &beat) != ESP_OK) {
            ++s_rejected;
            return;
        }
        ++s_hb_rx;
        uint32_t head = s_ring_head;
        uint32_t next = (head + 1) % PROBE_RX_RING;
        if (next == s_ring_tail) {
            ++s_ring_overflow;
            return;
        }
        s_ring[head].kind = PROBE_EVENT_HEARTBEAT;
        memcpy(s_ring[head].mac, mac, ESP_NOW_ETH_ALEN);
        s_ring[head].header = view.header;
        s_ring[head].heartbeat = beat;
        s_ring_head = next;
        return;
    }
    default:
        // The sender never sends key acks or heartbeats to itself; anything else is a
        // foreign device and is counted as rejected rather than silently dropped.
        ++s_rejected;
        return;
    }
}

static void reply_heartbeat(const uint8_t mac[ESP_NOW_ETH_ALEN], const mic_link_header_t *header,
                            const mic_link_heartbeat_t *beat)
{
    mic_link_heartbeat_ack_t ack = {
        .echo_seq = header->seq,
        .phase = beat->phase,
        .state = (uint8_t)(s_encrypted_peer ? MIC_LINK_STATE_PAIRED : 0),
        .flags = 0,
        .audio_rx = s_audio_rx,
        .audio_dup = s_audio_dup,
        .audio_gap = s_sessions.audio.holes,
        .audio_late = s_sessions.audio.late,
        .key_rx = s_key_rx,
        .key_dup = s_key_dup,
        .rejected = s_rejected,
    };

    mic_link_frame_t frame;
    if (mic_link_build_heartbeat_ack(&frame, s_peer_session, ++s_reply_seq, &ack) != ESP_OK) {
        return;
    }
    // ESP-NOW will not transmit to an address that is not in its peer list, so the
    // sender's unicast address has to be registered before the first reply. The probe
    // registers it as plaintext until pairing upgrades the entry to encrypted.
    if (mic_radio_set_peer(mac, s_encrypted_peer, s_peer_lmk) != ESP_OK) {
        return;
    }
    esp_err_t err = mic_radio_send(mac, frame.bytes, frame.len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Heartbeat reply failed: %s (0x%x)", esp_err_to_name(err), err);
    }
}

static void handle_pair_request(const probe_event_t *event)
{
    mic_link_pair_resp_t response;
    memset(&response, 0, sizeof(response));

    uint8_t current_channel = mic_radio_channel();

    if (event->pair.channel != current_channel) {
        // The sender proposed a different channel. Accepting would leave the two ends
        // on different channels with no way to discover it, so refuse explicitly.
        ESP_LOGE(TAG, "PROBE_PAIR_REJECT reason=channel_mismatch requested=%u current=%u",
                 (unsigned)event->pair.channel, (unsigned)current_channel);
        response.result = MIC_LINK_PAIR_RESULT_CHANNEL_MISMATCH;
        response.state = 0;
    } else {
        response.result = MIC_LINK_PAIR_RESULT_OK;
        response.state = MIC_LINK_STATE_PAIRED;
    }

    mic_link_frame_t frame;
    if (mic_link_build_pair_resp(&frame, &response) == ESP_OK) {
        mic_radio_send(event->mac, frame.bytes, frame.len);
    }
    if (response.result != MIC_LINK_PAIR_RESULT_OK) {
        return;
    }

    // Adopt the sender's keys. The peer entry is rebuilt as encrypted, which is what
    // makes every later frame from this sender require the link key.
    memcpy(s_peer_mac, event->mac, ESP_NOW_ETH_ALEN);
    s_peer_known = true;
    if (mic_radio_set_pmk(event->pair.pmk) != ESP_OK) {
        ESP_LOGE(TAG, "PROBE_PAIR result=fail reason=pmk");
        return;
    }
    memcpy(s_peer_lmk, event->pair.lmk, ESP_NOW_KEY_LEN);
    if (mic_radio_set_peer(event->mac, true, event->pair.lmk) != ESP_OK) {
        ESP_LOGE(TAG, "PROBE_PAIR result=fail reason=peer");
        return;
    }
    s_encrypted_peer = true;

    mic_bind_record_t record;
    memset(&record, 0, sizeof(record));
    record.paired = true;
    memcpy(record.peer_mac, event->mac, ESP_NOW_ETH_ALEN);
    record.channel = current_channel;
    memcpy(record.lmk, event->pair.lmk, ESP_NOW_KEY_LEN);
    memcpy(record.pmk, event->pair.pmk, ESP_NOW_KEY_LEN);
    record.generation = 1;
    esp_err_t err = mic_bind_store(&record);
    ESP_LOGI(TAG, "PROBE_PAIR result=ok encrypt=1 channel=%u persistence=%s",
             (unsigned)current_channel, err == ESP_OK ? "ok" : "failed");
}

static void receiver_task(void *argument)
{
    uint8_t channel = (uint8_t)(uintptr_t)argument;

    mic_radio_config_t config = {.channel = channel, .encrypt_unicast = false};
    esp_err_t err = mic_radio_start(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PROBE_FAIL stage=radio err=%s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    mic_radio_set_receiver(on_recv, NULL);

    // Report what the NVS already holds, without letting it change this run: the probe
    // always starts unbound so the plaintext phases are meaningful, and only pairing
    // adds a peer.
    // The probe always starts unbound: a leftover binding would make the plaintext
    // phases indistinguishable from a broken link, and it would hide whether the
    // pairing phase actually wrote anything. Erasing here is probe-only behaviour and
    // is exactly what product firmware must never do on a normal boot.
    mic_bind_record_t existing;
    esp_err_t bind_err = mic_bind_load(&existing);
    if (bind_err == ESP_OK) {
        ESP_LOGI(TAG, "PROBE_BIND state=present peer=" MACSTR " channel=%u generation=%" PRIu32
                      " action=erased_for_probe",
                 MAC2STR(existing.peer_mac), (unsigned)existing.channel, existing.generation);
        if (mic_bind_erase() != ESP_OK) {
            ESP_LOGE(TAG, "PROBE_BIND state=present action=erase_failed");
        }
    } else {
        ESP_LOGI(TAG, "PROBE_BIND state=absent reason=%s", esp_err_to_name(bind_err));
    }

    ESP_LOGI(TAG, "PROBE_READY role=receiver channel=%u protocol=%u window_ms=%u start_delay_ms=%u",
             (unsigned)channel, (unsigned)MIC_LINK_VERSION, (unsigned)PROBE_PAIR_WINDOW_MS,
             (unsigned)MIC_PROBE_START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(MIC_PROBE_START_DELAY_MS));
    // Repeated after the start delay for the same reason as the sender: a capture
    // normally begins after the READY line has already scrolled past.
    ESP_LOGI(TAG, "PROBE_ARMED role=receiver channel=%u protocol=%u window_ms=%u", (unsigned)channel,
             (unsigned)MIC_LINK_VERSION, (unsigned)PROBE_PAIR_WINDOW_MS);

    int64_t last_log_us = esp_timer_get_time();
    int64_t deadline = last_log_us + PROBE_PAIR_WINDOW_MS * 1000LL;

    while (esp_timer_get_time() < deadline) {
        bool idle = true;
        while (s_ring_tail != s_ring_head) {
            uint32_t tail = s_ring_tail;
            probe_event_t event = s_ring[tail];
            s_ring_tail = (tail + 1) % PROBE_RX_RING;
            idle = false;
            if (event.kind == PROBE_EVENT_HEARTBEAT) {
                reply_heartbeat(event.mac, &event.header, &event.heartbeat);
            } else {
                handle_pair_request(&event);
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - last_log_us >= PROBE_LOG_PERIOD_MS * 1000LL) {
            last_log_us = now;
            ESP_LOGI(TAG,
                     "PROBE_RX audio_rx=%" PRIu32 " audio_dup=%" PRIu32 " audio_gap=%" PRIu32
                     " audio_late=%" PRIu32 " payload_errors=%" PRIu32 " key_rx=%" PRIu32
                     " key_dup=%" PRIu32 " heartbeats=%" PRIu32 " rejected=%" PRIu32
                     " ring_overflow=%" PRIu32 " encrypted=%d",
                     s_audio_rx, s_audio_dup, s_sessions.audio.holes, s_sessions.audio.late,
                     s_audio_payload_errors, s_key_rx, s_key_dup, s_hb_rx, s_rejected,
                     s_ring_overflow, (int)s_encrypted_peer);
        }
        if (idle) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    mic_radio_stats_t radio;
    mic_radio_get_stats(&radio);
    ESP_LOGI(TAG,
             "PROBE_SUMMARY audio_rx=%" PRIu32 " audio_dup=%" PRIu32 " audio_holes=%" PRIu32
             " audio_late=%" PRIu32 " payload_errors=%" PRIu32 " key_rx=%" PRIu32
             " key_dup=%" PRIu32 " heartbeats=%" PRIu32 " rejected=%" PRIu32
             " session_changes=%" PRIu32 " encrypted=%d rx_frames=%" PRIu32 " rx_bytes=%" PRIu32
             " rssi_min=%d rssi_max=%d",
             s_audio_rx, s_audio_dup, s_sessions.audio.holes, s_sessions.audio.late,
             s_audio_payload_errors, s_key_rx, s_key_dup, s_hb_rx, s_rejected,
             s_sessions.session_changes, (int)s_encrypted_peer, radio.rx_frames, radio.rx_bytes,
             radio.min_rssi, radio.max_rssi);

    bool passed = s_audio_rx > 0 && s_audio_payload_errors == 0;
    ESP_LOGI(TAG, "PROBE_RESULT: %s audio_rx=%" PRIu32 " payload_errors=%" PRIu32
                  " encrypted=%d",
             passed ? "PASS" : "FAIL", s_audio_rx, s_audio_payload_errors, (int)s_encrypted_peer);
    vTaskDelete(NULL);
}

esp_err_t mic_probe_run_receiver(uint8_t channel)
{
    if (channel < 1 || channel > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTaskCreate(receiver_task, "probe_rx", 6144, (void *)(uintptr_t)channel, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
