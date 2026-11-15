// S3-side key path. See mic_link_keys.h for the contract.
//
// Two rules shape this file, and both come from failure modes that are invisible until
// they bite:
//
//  1. The ACK is sent only after the report that the event implies reached the USB stack.
//     Acknowledging earlier would let a keystroke disappear with no retransmission able to
//     repair it, because the sequence number is already marked applied.
//  2. Release happens on its own schedule, never as a side effect. Link timeout, a new
//     session, USB reset, detach and a mapping change all call release_all(), so the host
//     can never be left holding a key that no physical key is holding.

#include "mic_link_keys.h"

#include <inttypes.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mic_key_store.h"
#include "mic_session.h"
#include "usb_descriptors.h"

static const char *TAG = "s3_keys";

// Bounded on purpose: three human-paced keys cannot fill sixteen slots. Overflow means
// something is already wrong, so it is counted rather than grown around.
#define KEY_QUEUE_DEPTH 16
// How long a report may wait for the host to poll before it is retried on the next pass.
#define KEY_REPORT_WAIT_MS 20
// Applied-but-unacknowledged events waiting for a successful report. One per queue slot is
// enough: an event can only become unacknowledged by leaving the queue.
#define KEY_UNACKED_MAX KEY_QUEUE_DEPTH
// How long a KEY_ACK transmission may be retried before it is counted as failed. The
// sender retransmits its event when it sees no ACK, so losing one ACK is survivable.
#define KEY_ACK_ATTEMPTS 4
#define KEY_ACK_RETRY_MS 2

#define KEY_TASK_STACK 4096
#define KEY_TASK_PRIORITY 5

typedef struct {
    uint32_t seq;
    uint16_t session;  // the session the event arrived under; the ACK echoes it
    uint8_t key_id;
    uint8_t event;
} key_event_msg_t;

static QueueHandle_t s_queue;
static mic_key_state_t s_state;
static mic_key_map_t s_map;
static mic_link_key_stats_t s_stats;
static uint32_t s_ack_seq;

// Sequence numbers whose events are already reflected in the held state but whose report
// has not reached the host. A successful report carries the whole state, so one success
// clears all of them at once.
static uint32_t s_unacked[KEY_UNACKED_MAX];
static size_t s_unacked_count;

// Set from any context; acted on by the key task. Using a flag rather than calling into
// TinyUSB directly keeps every USB call on one task.
static volatile bool s_release_pending;
static volatile uint32_t s_session_resets;
// True while the held state has not been accepted by the host. A report issued while the
// device is unmounted or suspended cannot be delivered, and dropping it would leave the
// host's last known state as the truth forever; the key task therefore retries until a
// report lands.
static bool s_report_dirty;

static const char *key_id_name(uint8_t key_id)
{
    switch (key_id) {
    case MIC_KEY_ID_UP:
        return "up";
    case MIC_KEY_ID_DOWN:
        return "down";
    case MIC_KEY_ID_OK:
        return "ok";
    default:
        return "unknown";
    }
}

static void push_unacked(uint32_t seq)
{
    if (seq == 0) {
        return;
    }
    if (s_unacked_count == KEY_UNACKED_MAX) {
        // Dropping the oldest is safe: the report that clears the rest already carries
        // its effect, and an event that old has been retransmitted several times.
        memmove(&s_unacked[0], &s_unacked[1], sizeof(s_unacked[0]) * (KEY_UNACKED_MAX - 1));
        --s_unacked_count;
        ++s_stats.dropped;
    }
    s_unacked[s_unacked_count++] = seq;
}

static void send_key_ack(uint16_t session, uint32_t acked_seq)
{
    mic_link_frame_t frame;
    if (mic_link_build_key_ack(&frame, session, ++s_ack_seq, acked_seq) != ESP_OK) {
        return;
    }
    for (int attempt = 0; attempt < KEY_ACK_ATTEMPTS; ++attempt) {
        if (mic_session_send(frame.bytes, frame.len) == ESP_OK) {
            ++s_stats.acks_sent;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_ACK_RETRY_MS));
    }
    ++s_stats.ack_failures;
}

// Hand the current held state to the host. Returns true once the host accepted it.
static bool send_state_report(void)
{
    uint8_t report[MIC_HID_KEYBOARD_REPORT_BYTES];
    mic_key_state_report(&s_state, report);

    const uint8_t *keycodes = &report[2];
    bool waited = false;
    for (int elapsed = 0; elapsed <= KEY_REPORT_WAIT_MS; ++elapsed) {
        if (tud_hid_n_ready(MIC_HID_KEYBOARD_INSTANCE)) {
            if (tud_hid_n_keyboard_report(MIC_HID_KEYBOARD_INSTANCE, 0, report[0], keycodes)) {
                ++s_stats.reports;
                if (waited) {
                    // Counted once per report that had to wait for the host, not once per
                    // millisecond of waiting: the number is meant to say "the host was not
                    // polling", not to act as a delay measurement.
                    ++s_stats.reports_retried;
                }
                return true;
            }
        }
        if (!waited) {
            waited = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

// Flush the held state to the host and release the ACKs that state covers. Called after
// every state change and after any forced release.
// Session of the events currently covered by the un-acked set. All events in flight
// belong to one session (a session change drops the queue), so a single value is enough;
// it is refreshed by every applied event.
static volatile uint16_t s_event_session;

static void flush_reports(void)
{
    if (!send_state_report()) {
        // The host is not polling (unmounted, or a suspend). Keep the report and the events
        // it covers pending: the sender retransmits unacknowledged events, and the key task
        // retries the report on its next pass so the release still reaches the host when it
        // comes back.
        s_report_dirty = true;
        return;
    }
    s_report_dirty = false;
    uint16_t session = s_event_session;
    for (size_t i = 0; i < s_unacked_count; ++i) {
        send_key_ack(session, s_unacked[i]);
    }
    s_unacked_count = 0;
}

void mic_link_keys_on_event(uint16_t session, uint32_t seq, uint8_t key_id, uint8_t event)
{
    // Wi-Fi task context: no logging, no USB, no blocking.
    if (key_id != MIC_KEY_ID_UP && key_id != MIC_KEY_ID_DOWN && key_id != MIC_KEY_ID_OK) {
        ++s_stats.invalid;
        return;
    }
    if (event != MIC_KEY_EVENT_PRESS && event != MIC_KEY_EVENT_RELEASE) {
        ++s_stats.invalid;
        return;
    }
    key_event_msg_t message = {.seq = seq, .session = session, .key_id = key_id, .event = event};
    if (xQueueSend(s_queue, &message, 0) != pdTRUE) {
        ++s_stats.dropped;
        return;
    }
    ++s_stats.events_rx;
    ++s_stats.pending;
}

void mic_link_keys_note_duplicate(void)
{
    ++s_stats.duplicates;
}

void mic_link_keys_self_test(uint8_t key_id, uint8_t event)
{
    // seq 0 is the "locally generated" marker: the key task applies the event without
    // acknowledging anything, because no sender is waiting for an ACK.
    key_event_msg_t message = {.seq = 0, .key_id = key_id, .event = event};
    if (xQueueSend(s_queue, &message, 0) != pdTRUE) {
        ++s_stats.dropped;
    }
}

void mic_link_keys_release_all(const char *reason)
{
    s_release_pending = true;
    ESP_LOGW(TAG, "S3_KEYS: ALL_UP reason=%s", reason == NULL ? "unspecified" : reason);
}

void mic_link_keys_on_session_change(void)
{
    // Any queued event belongs to the session that just ended, and its sequence numbers
    // will be reused by the new one. Applying them would apply stale keys under fresh
    // numbers, which the dedup filter cannot catch.
    s_session_resets = s_session_resets + 1;
    mic_link_keys_release_all("session_new");
}

void mic_link_keys_on_link_lost(void)
{
    ++s_stats.timeouts;
    mic_link_keys_release_all("link_timeout");
}

const mic_key_map_t *mic_link_keys_map(void)
{
    return &s_map;
}

void mic_link_keys_log_map(const char *reason)
{
    ESP_LOGI(TAG,
             "S3_KEYS: MAP reason=%s gen=%u UP=mod=0x%02x key=0x%02x DOWN=mod=0x%02x key=0x%02x "
             "OK=mod=0x%02x key=0x%02x",
             reason == NULL ? "unspecified" : reason, (unsigned)s_stats.map_generation,
             (unsigned)mic_key_map_find(&s_map, MIC_KEY_ID_UP)->modifier,
             (unsigned)mic_key_map_find(&s_map, MIC_KEY_ID_UP)->keycode,
             (unsigned)mic_key_map_find(&s_map, MIC_KEY_ID_DOWN)->modifier,
             (unsigned)mic_key_map_find(&s_map, MIC_KEY_ID_DOWN)->keycode,
             (unsigned)mic_key_map_find(&s_map, MIC_KEY_ID_OK)->modifier,
             (unsigned)mic_key_map_find(&s_map, MIC_KEY_ID_OK)->keycode);
}

void mic_link_keys_on_usb_reset(void)
{
    // The host may have dropped its view of the keyboard across a reset, a detach or a
    // suspend, so the current held state has to be re-sent even if this end thinks the
    // host already knows it. Releasing first is the safe direction: a stuck modifier after
    // a replug is far worse than a lost keypress during it.
    s_release_pending = true;
}

esp_err_t mic_link_keys_apply_map(const mic_key_map_t *map)
{
    // Validate first: a rejected table must not release keys, because the device keeps
    // working with the old one and a release the host sees for no reason is a visible
    // glitch.
    esp_err_t err = mic_key_map_validate(map);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "S3_KEYS: MAP_REJECT reason=invalid err=%s", esp_err_to_name(err));
        return err;
    }
    err = mic_key_store_save(map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "S3_KEYS: MAP_REJECT reason=nvs err=%s", esp_err_to_name(err));
        return err;
    }

    // Every held key's meaning is about to change, so release before adopting the table
    // (AGENTS.md section 4). The release is emitted by the key task, which then reports
    // the new table's state.
    mic_link_keys_release_all("map_change");
    mic_key_state_set_map(&s_state, map);
    s_map = *map;
    ++s_stats.map_generation;
    mic_link_keys_log_map("apply");
    return ESP_OK;
}

esp_err_t mic_link_keys_reset_map(void)
{
    mic_key_map_t defaults;
    mic_key_map_defaults(&defaults);
    return mic_link_keys_apply_map(&defaults);
}

void mic_link_keys_get_stats(mic_link_key_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_stats;
    out->map_generation = s_stats.map_generation;
}

uint8_t mic_link_keys_down_mask(void)
{
    return s_state.down_mask;
}

bool mic_link_keys_idle(void)
{
    return !mic_key_state_any_down(&s_state) && uxQueueMessagesWaiting(s_queue) == 0 &&
           s_unacked_count == 0;
}

static void key_task(void *argument)
{
    (void)argument;

    int64_t last_pending_log_us = 0;

    for (;;) {
        if (s_release_pending) {
            s_release_pending = false;
            // A session change invalidates everything already queued; a plain release
            // (timeout, USB reset) does not, because those events are still valid.
            if (s_session_resets > 0) {
                --s_session_resets;
                xQueueReset(s_queue);
                s_stats.dropped += s_stats.pending;
                s_stats.pending = 0;
                s_unacked_count = 0;
            }
            if (mic_key_state_release_all(&s_state)) {
                ++s_stats.all_up;
            }
            flush_reports();
            continue;
        }

        key_event_msg_t message;
        if (xQueueReceive(s_queue, &message, pdMS_TO_TICKS(10)) != pdTRUE) {
            // Nothing arrived. Still try to flush anything the host has not accepted yet,
            // so a report blocked during a suspend goes out as soon as the host is back.
            if (s_report_dirty) {
                flush_reports();
                int64_t now = esp_timer_get_time();
                if (now - last_pending_log_us > 2000000) {
                    last_pending_log_us = now;
                    ESP_LOGW(TAG, "S3_KEYS: REPORT_BLOCKED pending=%u unacked=%u",
                             (unsigned)s_stats.pending, (unsigned)s_unacked_count);
                }
            }
            continue;
        }

        if (s_stats.pending > 0) {
            --s_stats.pending;
        }

        if (!mic_key_state_apply(&s_state, message.key_id, message.event)) {
            // A repeated press or release. The link's dedup filter is per sequence number
            // and cannot see this, so it is absorbed here. It still gets an ACK: the
            // sender's copy of that event is delivered, and re-ACKing costs one frame.
            // A locally generated event (seq 0) has nobody waiting for one.
            if (message.seq != 0) {
                s_event_session = message.session;
                send_key_ack(message.session, message.seq);
            }
            continue;
        }

        ++s_stats.events_applied;
        // One line per applied event: human-paced, and the only place that can pair a
        // physical press with the report it produced.
        ESP_LOGI(TAG, "S3_KEYS: EVENT key=%s event=%s seq=%" PRIu32 " down=0x%02x",
                 key_id_name(message.key_id),
                 message.event == MIC_KEY_EVENT_PRESS ? "press" : "release", message.seq,
                 (unsigned)s_state.down_mask);
        // A locally generated event has no sequence number, so there is nothing to
        // acknowledge when the report lands.
        if (message.seq != 0) {
            s_event_session = message.session;
            push_unacked(message.seq);
        }
        flush_reports();
    }
}

esp_err_t micclicker_s3_keys_start(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.map_generation = 0;
    s_unacked_count = 0;
    s_ack_seq = 0;

    esp_err_t err = mic_key_store_load(&s_map);
    if (err == ESP_OK) {
        mic_link_keys_log_map("nvs");
    } else if (err == ESP_ERR_NOT_FOUND) {
        mic_key_map_defaults(&s_map);
        mic_link_keys_log_map("defaults");
    } else {
        // A stored blob that does not parse is a corrupt record rather than a missing
        // one. Falling back to the defaults keeps the device usable, and the log line is
        // what tells an operator the record was replaced rather than read.
        ESP_LOGW(TAG, "S3_KEYS: MAP_LOAD_FAIL err=%s; using defaults", esp_err_to_name(err));
        mic_key_map_defaults(&s_map);
        mic_link_keys_log_map("defaults_after_error");
    }

    mic_key_state_init(&s_state, &s_map);

    s_queue = xQueueCreate(KEY_QUEUE_DEPTH, sizeof(key_event_msg_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "S3_KEYS: FAIL reason=queue");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(key_task, "mc_s3_keys", KEY_TASK_STACK, NULL, KEY_TASK_PRIORITY, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "S3_KEYS: FAIL reason=task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "S3_KEYS: READY queue=%d", KEY_QUEUE_DEPTH);
    return ESP_OK;
}
