// ESP-NOW probe, sender role (C3).
//
// Runs the stage-3 radio test as a fixed phase schedule and logs one machine-readable
// summary per phase. The schedule is deliberately deterministic so two runs can be
// compared and so a change in the numbers means a change in the radio, not in the
// traffic pattern.
//
// Nothing here is product code: the product firmware derives its traffic from a real
// microphone and a real button, while the probe substitutes a deterministic pattern so
// the receiver can verify payloads and count loss.

#include "mic_probe.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_bind.h"
#include "mic_link.h"
#include "mic_probe_shared.h"
#include "mic_radio.h"

static const char *TAG = "probe_tx";

// Phase shapes. These knobs define the test, so changing one changes what the numbers
// mean; firmware/tests/espnow/README.md records the values that produced the committed
// results.
#define PROBE_DISCOVERY_TIMEOUT_MS 5000
#define PROBE_DISCOVERY_TICK_MS 10
#define PROBE_DISCOVERY_INTERVAL_MS 200
#define PROBE_PAIR_TIMEOUT_MS 5000
#define PROBE_PAIR_INTERVAL_MS 300
#define PROBE_STREAM_DURATION_MS 3000
#define PROBE_STREAM_HEARTBEAT_EVERY 50
#define PROBE_STREAM_MAX_FRAMES 50000
#define PROBE_SPACED_FRAMES 250
#define PROBE_SPACED_PERIOD_MS 20
#define PROBE_KEY_EVENTS 20
#define PROBE_KEY_PACING_MS 20
#define PROBE_ACK_WAIT_MS 400
#define PROBE_ACK_RING 32

typedef struct {
    mic_link_heartbeat_ack_t ack;
    int32_t rtt_us;
} probe_ack_event_t;

// Single-producer (Wi-Fi task) / single-consumer (probe task) ring. The callback only
// copies one event and bumps the head, so it never blocks the Wi-Fi task even during
// the fastest phase.
static probe_ack_event_t s_ack_ring[PROBE_ACK_RING];
static volatile uint32_t s_ack_head;
static volatile uint32_t s_ack_tail;
static volatile uint32_t s_ack_overflow;

// Transmit times of outstanding heartbeats, indexed by seq % PROBE_ACK_RING. Written by
// the probe task before sending and read by the callback to compute RTT.
static int64_t s_hb_sent_us[PROBE_ACK_RING];

static uint16_t s_session;
static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static volatile bool s_peer_discovered;
static volatile uint8_t s_pair_result;
static volatile bool s_pair_replied;

static uint32_t s_audio_seq;
static uint32_t s_key_seq;
static uint32_t s_hb_seq;

typedef struct {
    uint32_t requested;
    uint32_t queued;
    uint32_t immediate_errors;
    uint32_t no_mem;
    uint32_t channel_errors;
    uint32_t other_errors;
    uint32_t acks;
    uint32_t rtt_min_us;
    uint32_t rtt_max_us;
    uint64_t rtt_sum_us;
    uint32_t rtt_count;
} probe_phase_stats_t;

static void stats_reset(probe_phase_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->rtt_min_us = UINT32_MAX;
}

static void stats_note_rtt(probe_phase_stats_t *stats, uint32_t rtt_us)
{
    if (rtt_us < stats->rtt_min_us) {
        stats->rtt_min_us = rtt_us;
    }
    if (rtt_us > stats->rtt_max_us) {
        stats->rtt_max_us = rtt_us;
    }
    stats->rtt_sum_us += rtt_us;
    ++stats->rtt_count;
}

static void on_recv(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint8_t *data, int len, int8_t rssi,
                    void *context)
{
    (void)rssi;
    (void)context;

    mic_link_view_t view;
    if (mic_link_parse(data, (size_t)len, &view) != ESP_OK) {
        // Rejected frames are counted by the radio layer; the probe does not salvage them.
        return;
    }

    if (view.header.type == MIC_LINK_TYPE_HEARTBEAT_ACK) {
        mic_link_heartbeat_ack_t ack;
        if (mic_link_decode_heartbeat_ack(&view, &ack) != ESP_OK) {
            return;
        }
        if (!s_peer_discovered) {
            // Discovery is broadcast, so the first reply is also how the sender learns
            // the receiver's unicast address.
            memcpy(s_peer_mac, mac, ESP_NOW_ETH_ALEN);
            s_peer_discovered = true;
        }
        uint32_t index = ack.echo_seq % PROBE_ACK_RING;
        int64_t sent_us = s_hb_sent_us[index];
        int32_t rtt_us = sent_us > 0 ? (int32_t)(esp_timer_get_time() - sent_us) : -1;

        uint32_t head = s_ack_head;
        uint32_t next = (head + 1) % PROBE_ACK_RING;
        if (next == s_ack_tail) {
            ++s_ack_overflow;
            return;
        }
        s_ack_ring[head].ack = ack;
        s_ack_ring[head].rtt_us = rtt_us;
        s_ack_head = next;
        return;
    }

    if (view.header.type == MIC_LINK_TYPE_PAIR_RESP) {
        mic_link_pair_resp_t resp;
        if (mic_link_decode_pair_resp(&view, &resp) == ESP_OK) {
            memcpy(s_peer_mac, mac, ESP_NOW_ETH_ALEN);
            s_pair_result = resp.result;
            s_pair_replied = true;
        }
    }
}

// Consume every queued reply. Returns how many were consumed; `last` receives the
// newest reply so the caller can diff the receiver's cumulative counters.
static uint32_t drain_acks(probe_phase_stats_t *stats, mic_link_heartbeat_ack_t *last)
{
    uint32_t consumed = 0;
    while (s_ack_tail != s_ack_head) {
        uint32_t tail = s_ack_tail;
        probe_ack_event_t event = s_ack_ring[tail];
        s_ack_tail = (tail + 1) % PROBE_ACK_RING;
        ++consumed;
        ++stats->acks;
        if (event.rtt_us >= 0) {
            stats_note_rtt(stats, (uint32_t)event.rtt_us);
        }
        if (last != NULL) {
            *last = event.ack;
        }
    }
    return consumed;
}

static esp_err_t send_frame(mic_link_frame_t *frame, bool broadcast, probe_phase_stats_t *stats)
{
    ++stats->requested;
    esp_err_t err = mic_radio_send(broadcast ? NULL : s_peer_mac, frame->bytes, frame->len);
    if (err == ESP_OK) {
        ++stats->queued;
        return ESP_OK;
    }
    ++stats->immediate_errors;
    if (err == ESP_ERR_ESPNOW_NO_MEM) {
        ++stats->no_mem;
    } else if (err == ESP_ERR_ESPNOW_CHAN) {
        ++stats->channel_errors;
    } else {
        ++stats->other_errors;
        ESP_LOGW(TAG, "Send failed: %s (0x%x)", esp_err_to_name(err), err);
    }
    return err;
}

static void send_heartbeat(uint8_t phase, uint8_t mode, bool broadcast, probe_phase_stats_t *stats)
{
    uint32_t seq = ++s_hb_seq;
    s_hb_sent_us[seq % PROBE_ACK_RING] = esp_timer_get_time();

    mic_link_heartbeat_t beat = {.phase = phase, .mode = mode, .flags = 0};
    mic_link_frame_t frame;
    if (mic_link_build_heartbeat(&frame, s_session, seq, &beat) == ESP_OK) {
        send_frame(&frame, broadcast, stats);
    }
}

// Discard replies still queued from an earlier phase.
//
// Without this, a phase's closing wait can be satisfied by the previous phase's reply
// and report that phase's cumulative counters as its own result. That produced a
// plausible-looking "receiver applied 0 key events" reading while the receiver had in
// fact applied all of them.
static void ack_ring_purge(void)
{
    s_ack_tail = s_ack_head;
}

// Block until the reply to `target_seq` arrives, or until PROBE_ACK_WAIT_MS passes.
// Matching on the echoed seq is what guarantees the snapshot belongs to this
// heartbeat; replies to older heartbeats are consumed and ignored.
static bool wait_for_ack_seq(probe_phase_stats_t *stats, uint32_t target_seq,
                             mic_link_heartbeat_ack_t *snapshot)
{
    int64_t deadline = esp_timer_get_time() + PROBE_ACK_WAIT_MS * 1000LL;
    bool matched = false;
    while (esp_timer_get_time() < deadline) {
        uint32_t before = stats->acks;
        drain_acks(stats, snapshot);
        if (stats->acks != before && snapshot != NULL && snapshot->echo_seq == target_seq) {
            matched = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return matched;
}

// Send one heartbeat and wait for that specific reply, so the returned snapshot is the
// receiver's counters as of this heartbeat rather than of some earlier one.
static bool heartbeat_snapshot(uint8_t phase, probe_phase_stats_t *stats,
                               mic_link_heartbeat_ack_t *snapshot)
{
    uint32_t seq = s_hb_seq + 1;
    send_heartbeat(phase, MIC_LINK_HEARTBEAT_MODE_PROBE, false, stats);
    return wait_for_ack_seq(stats, seq, snapshot);
}

// Same, but retried a few times with a pause between attempts.
//
// This matters right after the burst phase: while the sender is saturating the link,
// the receiver's Wi-Fi task is fully occupied with audio frames and its control-frame
// queue starves, so the single closing heartbeat can be dropped. Retrying after the
// flood has stopped is what makes the reported loss figure describe the audio stream
// rather than the probe's own ability to ask a question mid-flood.
static bool heartbeat_snapshot_retry(uint8_t phase, probe_phase_stats_t *stats,
                                     mic_link_heartbeat_ack_t *snapshot, int attempts)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (heartbeat_snapshot(phase, stats, snapshot)) {
            return true;
        }
    }
    return false;
}

static void log_phase_begin(uint8_t phase)
{
    ESP_LOGI(TAG, "PROBE_PHASE phase=%s id=%u state=begin", probe_phase_name(phase), (unsigned)phase);
}

static void log_phase_end(uint8_t phase, const probe_phase_stats_t *stats, uint32_t rx_loss,
                          uint32_t rx_dup)
{
    uint32_t rtt_avg = stats->rtt_count > 0 ? (uint32_t)(stats->rtt_sum_us / stats->rtt_count) : 0;
    ESP_LOGI(TAG,
             "PROBE_PHASE phase=%s id=%u state=end tx_requested=%" PRIu32 " tx_queued=%" PRIu32
             " tx_immediate_errors=%" PRIu32 " tx_no_mem=%" PRIu32 " tx_channel_errors=%" PRIu32
             " tx_other_errors=%" PRIu32 " acks=%" PRIu32 " peer_loss=%" PRIu32
             " peer_dup=%" PRIu32 " rtt_min_us=%" PRIu32 " rtt_avg_us=%" PRIu32
             " rtt_max_us=%" PRIu32 " rtt_samples=%" PRIu32 " ack_ring_overflow=%" PRIu32,
             probe_phase_name(phase), (unsigned)phase, stats->requested, stats->queued,
             stats->immediate_errors, stats->no_mem, stats->channel_errors, stats->other_errors,
             stats->acks, rx_loss, rx_dup, stats->rtt_count > 0 ? stats->rtt_min_us : 0, rtt_avg,
             stats->rtt_max_us, stats->rtt_count, s_ack_overflow);
}

static bool phase_discovery(probe_phase_stats_t *stats)
{
    log_phase_begin(PROBE_PHASE_DISCOVERY);
    int64_t deadline = esp_timer_get_time() + PROBE_DISCOVERY_TIMEOUT_MS * 1000LL;
    int64_t next_send_us = 0;
    while (esp_timer_get_time() < deadline && !s_peer_discovered) {
        if (esp_timer_get_time() >= next_send_us) {
            send_heartbeat(PROBE_PHASE_DISCOVERY, MIC_LINK_HEARTBEAT_MODE_DISCOVER, true, stats);
            next_send_us = esp_timer_get_time() + PROBE_DISCOVERY_INTERVAL_MS * 1000LL;
        }
        drain_acks(stats, NULL);
        vTaskDelay(pdMS_TO_TICKS(PROBE_DISCOVERY_TICK_MS));
    }
    bool found = s_peer_discovered;
    if (found) {
        // The reply that ended the loop is still queued: the `while` condition stopped
        // the loop before its next drain. Account for it here so the discovery phase
        // reports the ack and RTT it actually produced.
        drain_acks(stats, NULL);
        ESP_LOGI(TAG, "PROBE_PEER mac=" MACSTR " role=receiver", MAC2STR(s_peer_mac));
    } else {
        ESP_LOGE(TAG, "PROBE_PEER mac=00:00:00:00:00:00 role=none reason=discovery_timeout");
    }
    log_phase_end(PROBE_PHASE_DISCOVERY, stats, 0, 0);
    return found;
}

// Generate this pairing's keys on the device, so no link key is baked into the binary.
// The plaintext handoff this needs is recorded in docs/protocol.md.
static void generate_pair_request(mic_link_pair_req_t *request)
{
    uint8_t material[MIC_LINK_PAIR_KEY_BYTES * 2];
    esp_fill_random(material, sizeof(material));
    memcpy(request->lmk, material, MIC_LINK_PAIR_KEY_BYTES);
    memcpy(request->pmk, &material[MIC_LINK_PAIR_KEY_BYTES], MIC_LINK_PAIR_KEY_BYTES);
    request->channel = MIC_PROBE_CHANNEL;
}

static bool phase_pairing(probe_phase_stats_t *stats)
{

    mic_link_pair_req_t request;
    generate_pair_request(&request);

    mic_link_frame_t frame;
    esp_err_t err = mic_link_build_pair_req(&frame, &request);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PROBE_PAIR result=none reason=build_failed err=%s", esp_err_to_name(err));
        return false;
    }

    int64_t deadline = esp_timer_get_time() + PROBE_PAIR_TIMEOUT_MS * 1000LL;
    int64_t next_send_us = 0;
    while (esp_timer_get_time() < deadline && !s_pair_replied) {
        if (esp_timer_get_time() >= next_send_us) {
            // Broadcast: the receiver only answers pair requests it can hear, and at
            // this point neither end has accepted the other as a unicast peer yet.
            send_frame(&frame, true, stats);
            next_send_us = esp_timer_get_time() + PROBE_PAIR_INTERVAL_MS * 1000LL;
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_DISCOVERY_TICK_MS));
    }

    if (!s_pair_replied) {
        ESP_LOGE(TAG, "PROBE_PAIR result=none reason=timeout");
        return false;
    }
    if (s_pair_result != MIC_LINK_PAIR_RESULT_OK) {
        ESP_LOGE(TAG, "PROBE_PAIR result=%u reason=rejected", (unsigned)s_pair_result);
        return false;
    }

    // Only now can the unicast peer be created: it needs the receiver's MAC and the
    // keys the receiver just acknowledged.
    err = mic_radio_set_pmk(request.pmk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PROBE_PAIR result=none reason=pmk_failed");
        return false;
    }
    err = mic_radio_set_peer(s_peer_mac, true, request.lmk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PROBE_PAIR result=none reason=peer_failed");
        return false;
    }

    mic_bind_record_t record;
    memset(&record, 0, sizeof(record));
    record.paired = true;
    memcpy(record.peer_mac, s_peer_mac, ESP_NOW_ETH_ALEN);
    record.channel = request.channel;
    memcpy(record.lmk, request.lmk, ESP_NOW_KEY_LEN);
    memcpy(record.pmk, request.pmk, ESP_NOW_KEY_LEN);
    record.generation = 1;
    // A failed write is reported but does not abort the run: the encrypted phases can
    // still be measured, and the persistence check is its own finding.
    if (mic_bind_store(&record) != ESP_OK) {
        ESP_LOGE(TAG, "PROBE_PAIR persistence=failed");
    } else {
        ESP_LOGI(TAG, "PROBE_PAIR persistence=ok");
    }
    ESP_LOGI(TAG, "PROBE_PAIR result=ok encrypt=1 channel=%u", (unsigned)request.channel);
    return true;
}

// Send as fast as ESP-NOW accepts, for a fixed wall-clock window. This is the
// throughput and burst-loss phase: it is deliberately faster than the audio rate, so
// it stresses the send queue rather than modelling speech.
static void phase_stream(probe_phase_stats_t *stats, mic_link_heartbeat_ack_t *before,
                         uint32_t *frames_sent, uint32_t *verify_errors)
{
    *frames_sent = 0;
    *verify_errors = 0;

    mic_link_heartbeat_ack_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (before != NULL) {
        snapshot = *before;
    }

    log_phase_begin(PROBE_PHASE_AUDIO_STREAM);
    int64_t start_us = esp_timer_get_time();
    int64_t stop_us = start_us + PROBE_STREAM_DURATION_MS * 1000LL;

    uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES];
    while (esp_timer_get_time() < stop_us && *frames_sent < PROBE_STREAM_MAX_FRAMES) {
        // Build the candidate frame once and retry the same seq while the driver refuses
        // it. Advancing the sequence number on a rejected send would leave a permanent
        // hole that the receiver cannot distinguish from radio loss, so this phase would
        // report loss the radio never caused.
        uint32_t seq = s_audio_seq + 1;
        probe_fill_audio(payload, seq);
        mic_link_frame_t frame;
        esp_err_t build = mic_link_build_audio(&frame, s_session, seq, payload);
        if (build != ESP_OK) {
            ++*verify_errors;
            break;
        }
        esp_err_t err = send_frame(&frame, false, stats);
        if (err == ESP_OK) {
            s_audio_seq = seq;
            ++*frames_sent;
        }
        if (stats->requested % PROBE_STREAM_HEARTBEAT_EVERY == 0) {
            send_heartbeat(PROBE_PHASE_AUDIO_STREAM, MIC_LINK_HEARTBEAT_MODE_PROBE, false, stats);
            drain_acks(stats, &snapshot);
        }
        if (err == ESP_ERR_ESPNOW_NO_MEM) {
            // The driver queue is full. Back off briefly instead of spinning: a tight
            // retry loop starves the Wi-Fi task that would drain it. The next iteration
            // reuses the same seq, so backpressure does not turn into apparent loss.
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Closing snapshot, retried: the burst keeps the receiver's control path starved
    // until it stops, so one attempt is not enough to read the audio counters back.
    bool got_closing = heartbeat_snapshot_retry(PROBE_PHASE_AUDIO_STREAM, stats, &snapshot, 5);

    uint32_t loss = 0;
    uint32_t dup = 0;
    if (before != NULL && got_closing) {
        // The receiver's counters are cumulative, so the phase figure is the delta.
        loss = (snapshot.audio_gap - snapshot.audio_late) - (before->audio_gap - before->audio_late);
        dup = snapshot.audio_dup - before->audio_dup;
    }
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    uint32_t kbps = 0;
    if (elapsed_us > 0) {
        // 168 wire bytes per frame, 8 bits per byte, 1000 ms/s.
        kbps = (uint32_t)(((uint64_t)*frames_sent * MIC_LINK_AUDIO_FRAME_BYTES * 8 * 1000) /
                          (uint64_t)elapsed_us);
    }
    ESP_LOGI(TAG, "PROBE_STREAM frames=%" PRIu32 " elapsed_ms=%" PRIu32 " wire_kbps=%" PRIu32
                  " verify_errors=%" PRIu32 " closing_ack=%d receiver_rx=%" PRIu32,
             *frames_sent, (uint32_t)(elapsed_us / 1000), kbps, *verify_errors, (int)got_closing,
             got_closing ? snapshot.audio_rx : 0);
    log_phase_end(PROBE_PHASE_AUDIO_STREAM, stats, loss, dup);
}

// Send at exactly the audio cadence (20 ms) so the timing path is exercised the way
// the product will use it, not just the queue depth.
static void phase_spaced(probe_phase_stats_t *stats, mic_link_heartbeat_ack_t *before)
{
    log_phase_begin(PROBE_PHASE_AUDIO_SPACED);
    mic_link_heartbeat_ack_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (before != NULL) {
        snapshot = *before;
    }

    uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES];
    int64_t start_us = esp_timer_get_time();
    for (uint32_t i = 0; i < PROBE_SPACED_FRAMES; ++i) {
        uint32_t seq = s_audio_seq + 1;
        probe_fill_audio(payload, seq);
        mic_link_frame_t frame;
        if (mic_link_build_audio(&frame, s_session, seq, payload) == ESP_OK) {
            // At the audio cadence the driver never refuses a frame; if it ever does,
            // keeping the seq lets the next frame cover the gap instead of widening it.
            if (send_frame(&frame, false, stats) == ESP_OK) {
                s_audio_seq = seq;
            }
        }
        if (i % PROBE_STREAM_HEARTBEAT_EVERY == 0) {
            send_heartbeat(PROBE_PHASE_AUDIO_SPACED, MIC_LINK_HEARTBEAT_MODE_PROBE, false, stats);
            drain_acks(stats, &snapshot);
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_SPACED_PERIOD_MS));
    }
    bool got_closing = heartbeat_snapshot_retry(PROBE_PHASE_AUDIO_SPACED, stats, &snapshot, 3);

    uint32_t loss = 0;
    uint32_t dup = 0;
    if (before != NULL && got_closing) {
        loss = (snapshot.audio_gap - snapshot.audio_late) - (before->audio_gap - before->audio_late);
        dup = snapshot.audio_dup - before->audio_dup;
    }
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    ESP_LOGI(TAG, "PROBE_SPACED frames=%u elapsed_ms=%" PRIu32 " closing_ack=%d receiver_rx=%" PRIu32,
             (unsigned)PROBE_SPACED_FRAMES, (uint32_t)(elapsed_us / 1000), (int)got_closing,
             got_closing ? snapshot.audio_rx : 0);
    log_phase_end(PROBE_PHASE_AUDIO_SPACED, stats, loss, dup);
}

// Send key events without an ack listener, then verify the receiver's key counters
// moved by exactly the number of distinct events. The point is that retransmitting the
// same seq must not be applied twice.
static void phase_key_retry(probe_phase_stats_t *stats, mic_link_heartbeat_ack_t *before)
{
    log_phase_begin(PROBE_PHASE_KEY_RETRY);

    mic_link_heartbeat_ack_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (before != NULL) {
        snapshot = *before;
    }

    uint32_t events = 0;
    uint32_t retransmits = 0;
    for (uint32_t i = 0; i < PROBE_KEY_EVENTS; ++i) {
        uint32_t seq = ++s_key_seq;
        mic_link_key_t key = {
            .key_id = (uint8_t)(MIC_LINK_KEY_UP + (i % 3u)),
            .event = (uint8_t)((i % 2u) ? MIC_LINK_KEY_EVENT_RELEASE : MIC_LINK_KEY_EVENT_PRESS),
        };
        mic_link_frame_t frame;
        if (mic_link_build_key(&frame, s_session, seq, &key) != ESP_OK) {
            continue;
        }
        ++events;
        s_key_seq = seq;
        // Three transmissions of the same seq: the first must be applied, the two
        // repeats must be suppressed as duplicates.
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (send_frame(&frame, false, stats) == ESP_OK && attempt > 0) {
                ++retransmits;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_KEY_PACING_MS));
    }

    bool got_closing = heartbeat_snapshot_retry(PROBE_PHASE_KEY_RETRY, stats, &snapshot, 3);

    uint32_t key_rx = 0;
    uint32_t key_dup = 0;
    if (before != NULL && got_closing) {
        key_rx = snapshot.key_rx - before->key_rx;
        key_dup = snapshot.key_dup - before->key_dup;
    }
    ESP_LOGI(TAG, "PROBE_KEY events=%" PRIu32 " retransmits=%" PRIu32 " receiver_applied=%" PRIu32
                  " receiver_suppressed=%" PRIu32 " closing_ack=%d",
             events, retransmits, key_rx, key_dup, (int)got_closing);
    log_phase_end(PROBE_PHASE_KEY_RETRY, stats, 0, key_dup);
}

static void sender_task(void *argument)
{
    uint8_t channel = (uint8_t)(uintptr_t)argument;

    s_session = (uint16_t)(esp_random() & 0xFFFFu);
    s_peer_discovered = false;
    s_pair_replied = false;

    mic_radio_config_t config = {.channel = channel, .encrypt_unicast = false};
    esp_err_t err = mic_radio_start(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PROBE_FAIL stage=radio err=%s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    mic_radio_set_receiver(on_recv, NULL);

    ESP_LOGI(TAG, "PROBE_READY role=sender session=%u channel=%u protocol=%u start_delay_ms=%u",
             (unsigned)s_session, (unsigned)channel, (unsigned)MIC_LINK_VERSION,
             (unsigned)MIC_PROBE_START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(MIC_PROBE_START_DELAY_MS));
    // Repeat the run identity here. The READY line prints before the start delay, which
    // is usually before a host can open the console, so this second line is what a
    // capture actually contains.
    ESP_LOGI(TAG, "PROBE_ARMED role=sender session=%u channel=%u protocol=%u", (unsigned)s_session,
             (unsigned)channel, (unsigned)MIC_LINK_VERSION);

    probe_phase_stats_t stats;
    mic_link_heartbeat_ack_t baseline;
    memset(&baseline, 0, sizeof(baseline));

    stats_reset(&stats);
    bool discovered = phase_discovery(&stats);
    if (!discovered) {
        ESP_LOGE(TAG, "PROBE_RESULT: FAIL reason=no_receiver");
        vTaskDelete(NULL);
        return;
    }

    // ESP-NOW will not transmit to an address that is not in its peer list, and the
    // broadcast entry does not cover unicast. Every measured phase sends unicast, so
    // the discovered address has to be registered here, before the first audio frame.
    // It starts unencrypted because pairing happens last; phase_pairing upgrades this
    // same entry to encrypted.
    esp_err_t peer_err = mic_radio_set_peer(s_peer_mac, false, NULL);
    if (peer_err != ESP_OK) {
        ESP_LOGE(TAG, "PROBE_RESULT: FAIL reason=peer_register err=%s", esp_err_to_name(peer_err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "PROBE_PEER registered=1 encrypt=0");

    // A plaintext heartbeat first, to snapshot the receiver's counters at the start of
    // the measured phases.
    stats_reset(&stats);
    ack_ring_purge();
    heartbeat_snapshot(PROBE_PHASE_IDLE, &stats, &baseline);

    uint32_t stream_frames = 0;
    uint32_t stream_verify_errors = 0;
    stats_reset(&stats);
    ack_ring_purge();
    phase_stream(&stats, &baseline, &stream_frames, &stream_verify_errors);
    mic_link_heartbeat_ack_t after_stream = baseline;
    drain_acks(&stats, &after_stream);

    stats_reset(&stats);
    ack_ring_purge();
    phase_spaced(&stats, &after_stream);
    mic_link_heartbeat_ack_t after_spaced = after_stream;
    drain_acks(&stats, &after_spaced);

    stats_reset(&stats);
    ack_ring_purge();
    phase_key_retry(&stats, &after_spaced);

    // Pairing last, because it replaces the plaintext peer with an encrypted one and
    // every earlier phase depends on the plaintext path staying in place.
    stats_reset(&stats);
    ack_ring_purge();
    bool paired = phase_pairing(&stats);
    if (paired) {
        // The encrypted phase is measured only after pairing succeeded: before that
        // the peer is still plaintext, so a round trip would not prove encryption.
        stats_reset(&stats);
        ack_ring_purge();
        log_phase_begin(PROBE_PHASE_ENCRYPTED);
        mic_link_heartbeat_ack_t encrypted_ack;
        memset(&encrypted_ack, 0, sizeof(encrypted_ack));
        if (heartbeat_snapshot(PROBE_PHASE_ENCRYPTED, &stats, &encrypted_ack)) {
            ESP_LOGI(TAG, "PROBE_ENCRYPTED result=ok acks=%" PRIu32, stats.acks);
        } else {
            ESP_LOGE(TAG, "PROBE_ENCRYPTED result=fail reason=no_encrypted_reply");
        }
        log_phase_end(PROBE_PHASE_ENCRYPTED, &stats, 0, 0);
    }

    mic_radio_stats_t radio;
    mic_radio_get_stats(&radio);
    ESP_LOGI(TAG,
             "PROBE_RADIO tx_requests=%" PRIu32 " tx_ok=%" PRIu32 " tx_fail=%" PRIu32
             " tx_errors=%" PRIu32 " tx_bytes=%" PRIu32 " rx_frames=%" PRIu32 " rx_bytes=%" PRIu32
             " rssi_last=%d rssi_min=%d rssi_max=%d",
             radio.tx_requests, radio.tx_ok, radio.tx_fail, radio.tx_errors, radio.tx_bytes,
             radio.rx_frames, radio.rx_bytes, radio.last_rssi, radio.min_rssi, radio.max_rssi);

    ESP_LOGI(TAG, "PROBE_RESULT: %s stream_frames=%" PRIu32 " encrypted=%d", paired ? "PASS" : "FAIL",
             stream_frames, (int)paired);
    vTaskDelete(NULL);
}

esp_err_t mic_probe_run_sender(uint8_t channel)
{
    if (channel < 1 || channel > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTaskCreate(sender_task, "probe_tx", 6144, (void *)(uintptr_t)channel, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
