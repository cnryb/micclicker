// C3 product role: sample the button ladder, debounce it, deliver key events.
//
// The loop is paced by its own 10 ms tick rather than by the audio frame, because the two
// have nothing to do with each other: the audio path is clocked by the codec DMA and the
// keys by a human. Mixing them would make key latency depend on the microphone, which is
// exactly the wrong dependency.
//
// Delivery follows docs/protocol.md section 3.2:
//
//  * one press and one release are separate events with separate sequence numbers;
//  * an event that is not acknowledged is retransmitted with the same sequence number;
//  * a send the radio refused does not spend an attempt, because it never reached the air
//    and counting it would make a busy queue look like radio loss.
//
// The heartbeat is here as well. It is the only traffic that proves the link is alive when
// nobody is pressing keys, and it is what lets the S3 side tell "quiet" from "dead" and
// release keys accordingly.

#include "key_task.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_key_scan.h"
#include "mic_key_tx.h"
#include "mic_link.h"
#include "mic_session.h"

static const char *TAG = "c3_keys";

// One sample per tick. Slower than the ADC can go and faster than the 40 ms confirm
// window needs, so a debounced press is reported within 40-50 ms of the contact closing.
#define KEY_SAMPLE_PERIOD_MS 10
// The heartbeat doubles as the link's liveness signal, so its period is also the
// resolution of the receiver's timeout.
#define KEY_HEARTBEAT_PERIOD_MS 1000
// A heartbeat whose reply does not arrive within this window is treated as missed (the
// reply itself is never retransmitted: the next heartbeat is cheaper than repairing one).
#define KEY_HEARTBEAT_TIMEOUT_MS 2000

#define KEY_TASK_STACK 4096
#define KEY_TASK_PRIORITY 4

typedef struct {
    uint32_t samples;
    // Observed voltage span since boot. It is the cheapest way to tell "the ladder reads a
    // sensible released value" from "the ADC is misconfigured", and it costs two compares
    // per sample.
    int mv_min;
    int mv_max;
    uint32_t read_errors;
    uint32_t events_emitted;
    uint32_t heartbeats_sent;
    uint32_t heartbeat_acks;
    uint32_t heartbeat_missed;
    uint32_t send_refused;
} c3_key_stats_t;

static c3_key_stats_t s_stats;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_calibrated;
static mic_key_scan_t s_scan;
static mic_key_tx_t s_tx;
static uint32_t s_heartbeat_seq;

// Last heartbeat awaiting a reply. Only the task writes it; the receive callback reads it
// and converts it into a "replied" flag, so no lock is needed for a single word.
static volatile uint32_t s_heartbeat_outstanding;
static volatile bool s_heartbeat_replied;

// The link session every outbound frame carries. Set once at start-up from the audio
// path's number; key and heartbeat frames share it because the receiver keys its sequence
// trackers and its all-keys-up decisions off this field.
static uint16_t s_session;

static const char *key_name(uint8_t key_id)
{
    switch (key_id) {
    case MIC_KEY_SLOT_UP:
        return "up";
    case MIC_KEY_SLOT_DOWN:
        return "down";
    case MIC_KEY_SLOT_OK:
        return "ok";
    default:
        return "none";
    }
}

static const char *event_name(uint8_t event)
{
    return event == MIC_KEY_EVENT_PRESS ? "press" : "release";
}

static const mic_key_windows_t s_windows = {
    .up_max_mv = MIC_BOARD_BUTTON_MV_UP_MAX,
    .down_max_mv = MIC_BOARD_BUTTON_MV_DOWN_MAX,
    .ok_max_mv = MIC_BOARD_BUTTON_MV_OK_MAX,
};

static esp_err_t adc_open(void)
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = MIC_BOARD_BUTTON_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_config, &s_adc);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        // 12 dB attenuation gives the full ~3.3 V range, which the ladder needs: the
        // released state sits at the rail and every window boundary is above 0 V.
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, MIC_BOARD_BUTTON_ADC_CHANNEL, &channel_config);
    if (err != ESP_OK) {
        return err;
    }

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = MIC_BOARD_BUTTON_ADC_UNIT,
        .chan = MIC_BOARD_BUTTON_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali);
    if (err == ESP_OK) {
        s_calibrated = true;
    } else {
        // Without calibration the windows can still be applied to a linear estimate of
        // the raw reading, but the estimate is only good to tens of millivolts. Saying so
        // once is what keeps a misclassification from looking like a firmware bug.
        s_cali = NULL;
        s_calibrated = false;
        ESP_LOGW(TAG, "C3_KEYS: calibration unavailable err=%s; windows applied to raw",
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

static esp_err_t adc_sample_mv(int *millivolts)
{
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc, MIC_BOARD_BUTTON_ADC_CHANNEL, &raw);
    if (err != ESP_OK) {
        return err;
    }
    if (s_calibrated) {
        return adc_cali_raw_to_voltage(s_cali, raw, millivolts);
    }
    // The channel is configured for 12 dB attenuation over a 12-bit converter, so the
    // rail-to-rail span is 0..4095 against roughly 3.3 V.
    *millivolts = (raw * 3300) / 4095;
    return ESP_OK;
}

void micclicker_c3_keys_on_ack(uint32_t acked_seq)
{
    // Wi-Fi task context: mic_key_tx_ack only walks a small fixed array of pending
    // events. It does not log, allocate or send.
    mic_key_tx_ack(&s_tx, acked_seq);
}

void micclicker_c3_keys_on_heartbeat_ack(uint32_t echo_seq)
{
    if (echo_seq != 0 && echo_seq == s_heartbeat_outstanding) {
        s_heartbeat_replied = true;
        ++s_stats.heartbeat_acks;
    }
}

void micclicker_c3_keys_log_status(void)
{
    mic_key_tx_stats_t tx;
    mic_key_tx_get_stats(&s_tx, &tx);
    ESP_LOGI(TAG,
             "C3_KEYS: STATUS samples=%" PRIu32 " read_err=%" PRIu32 " events=%" PRIu32
             " submitted=%" PRIu32 " tx=%" PRIu32 " retx=%" PRIu32 " ack=%" PRIu32
             " expired=%" PRIu32 " dropped=%" PRIu32 " pending=%u hb=%" PRIu32
             " hb_ack=%" PRIu32 " hb_miss=%" PRIu32 " refused=%" PRIu32
             " mv=%d..%d held=0x%02x",
             s_stats.samples, s_stats.read_errors, s_stats.events_emitted, tx.submitted,
             tx.transmissions, tx.retransmits, tx.acknowledged, tx.expired, tx.dropped,
             (unsigned)mic_key_tx_pending(&s_tx), s_stats.heartbeats_sent, s_stats.heartbeat_acks,
             s_stats.heartbeat_missed, s_stats.send_refused, s_stats.mv_min, s_stats.mv_max,
             (unsigned)s_scan.held);
}

static void transmit_due_events(void)
{
    mic_key_tx_due_t due[MIC_KEY_TX_PENDING_MAX];
    size_t count = mic_key_tx_collect_due(&s_tx, esp_timer_get_time(), due,
                                          MIC_KEY_TX_PENDING_MAX);
    for (size_t i = 0; i < count; ++i) {
        mic_link_frame_t frame;
        mic_link_key_t key = {.key_id = due[i].key_id, .event = due[i].event};
        // The frame is rebuilt from the pending slot every time rather than cached,
        // because a cached copy is the one thing that would let a retransmission differ
        // from the original under a future field addition.
        if (mic_link_build_key(&frame, s_session, due[i].seq, &key) != ESP_OK) {
            continue;
        }
        if (mic_session_send(frame.bytes, frame.len) != ESP_OK) {
            // The radio refused it, so it never reached the air. Returning the attempt is
            // what keeps "queue was busy" from being counted as "the receiver missed it".
            mic_key_tx_undefer(&s_tx, due[i].seq, esp_timer_get_time());
            ++s_stats.send_refused;
        }
    }
    mic_key_tx_expire(&s_tx);
}

static void send_heartbeat(void)
{
    mic_link_heartbeat_t beat = {
        .phase = 0,
        .mode = MIC_LINK_HEARTBEAT_MODE_PROBE,
        .flags = 0,
    };
    // A dedicated counter but the shared session number: heartbeat is its own sequence
    // stream (docs/protocol.md section 2), yet it belongs to the same session, because the
    // receiver resets trackers on a session change and must see all traffic as one boot.
    uint32_t seq = ++s_heartbeat_seq;
    mic_link_frame_t frame;
    if (mic_link_build_heartbeat(&frame, s_session, seq, &beat) != ESP_OK) {
        return;
    }
    if (mic_session_send(frame.bytes, frame.len) != ESP_OK) {
        // Not paired or the radio refused. The next tick tries again; a heartbeat is not
        // worth a retry loop, because a missing one only delays the timeout decision.
        return;
    }

    if (s_heartbeat_outstanding != 0 && !s_heartbeat_replied) {
        ++s_stats.heartbeat_missed;
    }
    s_heartbeat_outstanding = seq;
    s_heartbeat_replied = false;
    ++s_stats.heartbeats_sent;
}

static void key_task(void *argument)
{
    (void)argument;

    int64_t next_sample_us = esp_timer_get_time();
    int64_t next_heartbeat_us = esp_timer_get_time() + (int64_t)KEY_HEARTBEAT_PERIOD_MS * 1000;
    int64_t heartbeat_sent_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();

        if (now >= next_sample_us) {
            next_sample_us += (int64_t)KEY_SAMPLE_PERIOD_MS * 1000;
            // A late tick must not be repaid with a burst: catching up would sample the
            // same contact several times in a row and shorten the debounce window.
            if (next_sample_us < now) {
                next_sample_us = now + (int64_t)KEY_SAMPLE_PERIOD_MS * 1000;
            }

            int millivolts = 0;
            if (adc_sample_mv(&millivolts) != ESP_OK) {
                ++s_stats.read_errors;
            } else {
                ++s_stats.samples;
                if (millivolts < s_stats.mv_min) {
                    s_stats.mv_min = millivolts;
                }
                if (millivolts > s_stats.mv_max) {
                    s_stats.mv_max = millivolts;
                }
                uint8_t slot = mic_key_classify(&s_windows, millivolts);
                mic_key_event_out_t events[2];
                size_t emitted = mic_key_scan_feed(&s_scan, slot, now / 1000, events);
                for (size_t i = 0; i < emitted; ++i) {
                    // MIC_KEY_SLOT_* and MIC_LINK_KEY_* share values on purpose, so the
                    // scanner's result is the wire id and no translation table exists to
                    // drift out of sync.
                    uint32_t seq = mic_key_tx_submit(&s_tx, events[i].key_id, events[i].event);
                    if (seq != 0) {
                        ++s_stats.events_emitted;
                        // Human-paced, so one line per event is a diagnostic rather than
                        // log pressure, and it is what lets a capture pair a press here
                        // with the report the host saw.
                        ESP_LOGI(TAG, "C3_KEYS: EVENT key=%s event=%s seq=%" PRIu32 " mv=%d",
                                 key_name(events[i].key_id), event_name(events[i].event), seq,
                                 millivolts);
                    }
                }
            }

            transmit_due_events();
        }

        if (now >= next_heartbeat_us) {
            next_heartbeat_us = now + (int64_t)KEY_HEARTBEAT_PERIOD_MS * 1000;
            send_heartbeat();
            heartbeat_sent_us = now;
        }

        // An outstanding heartbeat older than the window is declared missed instead of
        // waiting forever: the count is what a capture uses to see a one-way link.
        if (s_heartbeat_outstanding != 0 && !s_heartbeat_replied &&
            now - heartbeat_sent_us > (int64_t)KEY_HEARTBEAT_TIMEOUT_MS * 1000) {
            s_heartbeat_outstanding = 0;
            ++s_stats.heartbeat_missed;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void micclicker_c3_keys_set_session(uint16_t session)
{
    s_session = session;
}

esp_err_t micclicker_c3_keys_start(uint16_t session)
{
    s_session = session;
    esp_err_t err = adc_open();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C3_KEYS: FAIL reason=adc err=%s", esp_err_to_name(err));
        return err;
    }

    mic_key_scan_init(&s_scan, &s_windows, MIC_BOARD_BUTTON_CONFIRM_MS,
                      MIC_BOARD_BUTTON_HOLD_MS);
    mic_key_tx_init(&s_tx, MIC_KEY_TX_MAX_ATTEMPTS, MIC_KEY_TX_RETRY_MS);
    s_stats.mv_min = INT_MAX;
    s_stats.mv_max = INT_MIN;

    if (xTaskCreate(key_task, "mc_c3_keys", KEY_TASK_STACK, NULL, KEY_TASK_PRIORITY, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "C3_KEYS: FAIL reason=task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "C3_KEYS: READY sample_ms=%d confirm_ms=%d hold_ms=%d retry_ms=%d attempts=%d "
             "windows=%d/%d/%dmv",
             KEY_SAMPLE_PERIOD_MS, MIC_BOARD_BUTTON_CONFIRM_MS, MIC_BOARD_BUTTON_HOLD_MS,
             MIC_KEY_TX_RETRY_MS, MIC_KEY_TX_MAX_ATTEMPTS, s_windows.up_max_mv,
             s_windows.down_max_mv, s_windows.ok_max_mv);
    return ESP_OK;
}
