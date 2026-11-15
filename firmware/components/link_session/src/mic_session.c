// Product-side link bring-up. See mic_session.h for the contract.
//
// The two pairing roles differ in who owns the keys. The initiator generates them and
// persists them after the peer acknowledges; the responder adopts whatever the
// initiator sent. Both persist exactly one blob via link_bind, so an interrupted
// pairing leaves either the old record or no record, never half a binding.

#include "mic_session.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_link.h"
#include "mic_radio.h"

static const char *TAG = "mic_session";

static mic_session_info_t s_info;
static bool s_ready;
static volatile uint32_t s_pair_requests_seen;

// Set by the receive callback when a pairing response arrives, so the initiator's
// wait loop can stop as soon as the answer lands rather than at the deadline.
static volatile bool s_pair_replied;
static volatile uint8_t s_pair_result;
static volatile uint8_t s_pair_mac[ESP_NOW_ETH_ALEN];

// The responder's side of the same exchange: a request the callback parked for the
// pairing window to handle on the caller's task, because adopting it writes NVS and
// that must not happen in the Wi-Fi task.
static volatile bool s_pair_request_pending;
static volatile uint8_t s_pending_mac[ESP_NOW_ETH_ALEN];
static volatile mic_link_pair_req_t s_pending_request;

// The product has a single receive sink, installed by mic_session_set_receiver. This
// trampoline lets the session layer intercept pairing frames without knowing anything
// else about the caller's traffic.
static mic_radio_recv_fn s_user_receiver;
static void *s_user_context;

static void on_recv(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint8_t *data, int len, int8_t rssi,
                    void *context)
{
    (void)context;

    mic_link_view_t view;
    if (mic_link_parse(data, (size_t)len, &view) != ESP_OK) {
        // Validation belongs to the layer that owns the traffic; hand malformed frames
        // to the caller so its own `rejected` counter stays meaningful.
        if (s_user_receiver != NULL) {
            s_user_receiver(mac, data, len, rssi, s_user_context);
        }
        return;
    }

    if (view.header.type == MIC_LINK_TYPE_PAIR_RESP) {
        mic_link_pair_resp_t response;
        if (mic_link_decode_pair_resp(&view, &response) == ESP_OK) {
            memcpy((void *)s_pair_mac, mac, ESP_NOW_ETH_ALEN);
            s_pair_result = response.result;
            s_pair_replied = true;
        }
        return;
    }

    if (view.header.type == MIC_LINK_TYPE_PAIR_REQ) {
        mic_link_pair_req_t request;
        if (mic_link_decode_pair_req(&view, &request) == ESP_OK) {
            memcpy((void *)s_pending_mac, mac, ESP_NOW_ETH_ALEN);
            s_pending_request = request;
            s_pair_request_pending = true;
            ++s_pair_requests_seen;
        }
        return;
    }

    if (s_user_receiver != NULL) {
        s_user_receiver(mac, data, len, rssi, s_user_context);
    }
}

void mic_session_get(mic_session_info_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_info;
}

bool mic_session_ready(void)
{
    return s_ready;
}

esp_err_t mic_session_send(const uint8_t *data, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return mic_radio_send(s_info.peer_mac, data, len);
}

// Apply a binding to the radio. Shared by the boot path and by both pairing paths so
// there is exactly one place that decides what "paired" means to the radio.
static esp_err_t apply_binding(const mic_bind_record_t *record)
{
    esp_err_t err = mic_radio_set_pmk(record->pmk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LINK_SESSION apply=fail reason=pmk err=%s", esp_err_to_name(err));
        return err;
    }
    err = mic_radio_set_peer(record->peer_mac, true, record->lmk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LINK_SESSION apply=fail reason=peer err=%s", esp_err_to_name(err));
        return err;
    }

    s_info.paired = true;
    memcpy(s_info.peer_mac, record->peer_mac, ESP_NOW_ETH_ALEN);
    s_info.channel = record->channel;
    s_info.generation = record->generation;
    s_ready = true;
    s_info.state = MIC_SESSION_STATE_READY;
    return ESP_OK;
}

esp_err_t mic_session_start(uint8_t fallback_channel)
{
    memset(&s_info, 0, sizeof(s_info));
    s_ready = false;
    s_info.state = MIC_SESSION_STATE_UNBOUND;
    s_info.channel = fallback_channel;

    // NVS has to be up before the binding can be read, but mic_radio_start() cannot run
    // yet: it needs the channel, and the channel comes from the binding. Initialising
    // NVS here breaks that cycle. It is idempotent, so radio start's own call is
    // harmless. Never erase on failure: the partition also holds RF calibration and,
    // on a paired unit, the binding (docs/protocol.md invariant 2).
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "LINK_SESSION start=fail reason=nvs_init err=%s", esp_err_to_name(nvs_err));
        s_info.state = MIC_SESSION_STATE_ERROR;
        return nvs_err;
    }

    mic_bind_record_t record;
    esp_err_t bind_err = mic_bind_load(&record);

    // The channel has to be decided before the radio starts: the peer records the
    // channel it paired on, and ESP-NOW rejects a send whose peer disagrees.
    uint8_t channel = fallback_channel;
    if (bind_err == ESP_OK && record.paired) {
        channel = record.channel;
    }

    mic_radio_config_t config = {.channel = channel, .encrypt_unicast = true};
    esp_err_t err = mic_radio_start(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LINK_SESSION start=fail reason=radio err=%s", esp_err_to_name(err));
        s_info.state = MIC_SESSION_STATE_ERROR;
        return err;
    }

    if (bind_err == ESP_OK && record.paired) {
        err = apply_binding(&record);
        if (err != ESP_OK) {
            s_info.state = MIC_SESSION_STATE_ERROR;
            return err;
        }
        ESP_LOGI(TAG, "LINK_SESSION state=ready channel=%u generation=%" PRIu32 " peer=%02x%02x%02x%02x%02x%02x",
                 (unsigned)s_info.channel, s_info.generation, s_info.peer_mac[0],
                 s_info.peer_mac[1], s_info.peer_mac[2], s_info.peer_mac[3], s_info.peer_mac[4],
                 s_info.peer_mac[5]);
        return ESP_OK;
    }

    if (bind_err != ESP_ERR_NOT_FOUND) {
        // A record exists but cannot be read. Report it rather than silently treating
        // it as unbound, because the difference is a corrupted NVS or a version skew.
        ESP_LOGW(TAG, "LINK_SESSION state=unbound reason=bind_load err=%s", esp_err_to_name(bind_err));
    } else {
        ESP_LOGI(TAG, "LINK_SESSION state=unbound channel=%u", (unsigned)channel);
    }
    s_info.state = MIC_SESSION_STATE_UNBOUND;
    return ESP_OK;
}

esp_err_t mic_session_set_receiver(mic_radio_recv_fn callback, void *context)
{
    s_user_receiver = callback;
    s_user_context = context;
    return mic_radio_set_receiver(on_recv, NULL);
}

esp_err_t mic_session_pair_as_initiator(uint8_t channel)
{
    // Fresh keys per pairing keep link secrets out of the repository and out of the
    // build. docs/protocol.md section 3.5 records the plaintext-handoff cost of this.
    mic_link_pair_req_t request;
    uint8_t material[MIC_LINK_PAIR_KEY_BYTES * 2];
    esp_fill_random(material, sizeof(material));
    memcpy(request.lmk, material, MIC_LINK_PAIR_KEY_BYTES);
    memcpy(request.pmk, &material[MIC_LINK_PAIR_KEY_BYTES], MIC_LINK_PAIR_KEY_BYTES);
    request.channel = channel;

    mic_link_frame_t frame;
    esp_err_t err = mic_link_build_pair_req(&frame, &request);
    if (err != ESP_OK) {
        return err;
    }

    s_pair_replied = false;
    s_pair_result = MIC_LINK_PAIR_RESULT_REJECTED;
    s_info.state = MIC_SESSION_STATE_PAIRING;

    ESP_LOGI(TAG, "LINK_SESSION pair=initiator channel=%u window_ms=%d", (unsigned)channel,
             MIC_SESSION_PAIR_WINDOW_MS);

    int64_t deadline = esp_timer_get_time() + (int64_t)MIC_SESSION_PAIR_WINDOW_MS * 1000;
    int64_t next_send_us = 0;
    while (esp_timer_get_time() < deadline && !s_pair_replied) {
        int64_t now = esp_timer_get_time();
        if (now >= next_send_us) {
            // Broadcast: neither end has accepted the other as a unicast peer yet.
            mic_radio_send(MIC_RADIO_BROADCAST_MAC, frame.bytes, frame.len);
            next_send_us = now + (int64_t)MIC_SESSION_PAIR_INTERVAL_MS * 1000;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!s_pair_replied) {
        ESP_LOGE(TAG, "LINK_SESSION pair=initiator result=timeout");
        s_info.state = MIC_SESSION_STATE_UNBOUND;
        return ESP_ERR_TIMEOUT;
    }
    if (s_pair_result != MIC_LINK_PAIR_RESULT_OK) {
        ESP_LOGE(TAG, "LINK_SESSION pair=initiator result=rejected code=%u", (unsigned)s_pair_result);
        s_info.state = MIC_SESSION_STATE_UNBOUND;
        return ESP_ERR_INVALID_RESPONSE;
    }

    mic_bind_record_t record;
    memset(&record, 0, sizeof(record));
    record.paired = true;
    memcpy(record.peer_mac, (const void *)s_pair_mac, ESP_NOW_ETH_ALEN);
    record.channel = channel;
    memcpy(record.lmk, request.lmk, ESP_NOW_KEY_LEN);
    memcpy(record.pmk, request.pmk, ESP_NOW_KEY_LEN);
    record.generation = s_info.generation + 1;

    err = apply_binding(&record);
    if (err != ESP_OK) {
        s_info.state = MIC_SESSION_STATE_ERROR;
        return err;
    }

    err = mic_bind_store(&record);
    if (err != ESP_OK) {
        // The link works for this session but will not survive a reboot. That is worth
        // reporting as a failure, because the next boot will look unbound.
        ESP_LOGE(TAG, "LINK_SESSION pair=initiator persistence=failed err=%s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "LINK_SESSION pair=initiator result=ok channel=%u generation=%" PRIu32,
             (unsigned)channel, record.generation);
    return ESP_OK;
}

esp_err_t mic_session_pair_as_responder(uint32_t window_ms)
{
    if (window_ms == 0) {
        window_ms = MIC_SESSION_PAIR_WINDOW_MS;
    }

    s_pair_request_pending = false;
    s_info.state = MIC_SESSION_STATE_PAIRING;
    ESP_LOGI(TAG, "LINK_SESSION pair=responder window=%s",
             window_ms == MIC_SESSION_PAIR_FOREVER ? "forever" : "bounded");

    const bool forever = (window_ms == MIC_SESSION_PAIR_FOREVER);
    int64_t deadline = forever ? INT64_MAX : esp_timer_get_time() + (int64_t)window_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (s_pair_request_pending) {
            s_pair_request_pending = false;

            mic_link_pair_req_t request = s_pending_request;
            uint8_t mac[ESP_NOW_ETH_ALEN];
            memcpy(mac, (const void *)s_pending_mac, ESP_NOW_ETH_ALEN);

            mic_link_pair_resp_t response;
            memset(&response, 0, sizeof(response));
            uint8_t current_channel = mic_radio_channel();

            if (request.channel != current_channel) {
                // Accepting would leave the two ends on different channels with no way
                // to notice. Refuse explicitly and let each end log its own channel.
                ESP_LOGW(TAG, "LINK_SESSION pair=responder result=channel_mismatch want=%u have=%u",
                         (unsigned)request.channel, (unsigned)current_channel);
                response.result = MIC_LINK_PAIR_RESULT_CHANNEL_MISMATCH;
            } else {
                response.result = MIC_LINK_PAIR_RESULT_OK;
                response.state = MIC_LINK_STATE_PAIRED;
            }

            mic_link_frame_t frame;
            if (mic_link_build_pair_resp(&frame, &response) == ESP_OK) {
                // The reply is unicast, so the initiator's address must already be a
                // peer of this end. registered plaintext here precisely because the
                // keys are not applied yet.
                mic_radio_set_peer(mac, false, NULL);
                mic_radio_send(mac, frame.bytes, frame.len);
            }

            if (response.result != MIC_LINK_PAIR_RESULT_OK) {
                continue;  // keep the window open for a request on the right channel
            }

            mic_bind_record_t record;
            memset(&record, 0, sizeof(record));
            record.paired = true;
            memcpy(record.peer_mac, mac, ESP_NOW_ETH_ALEN);
            record.channel = current_channel;
            memcpy(record.lmk, request.lmk, ESP_NOW_KEY_LEN);
            memcpy(record.pmk, request.pmk, ESP_NOW_KEY_LEN);
            record.generation = s_info.generation + 1;

            esp_err_t err = apply_binding(&record);
            if (err != ESP_OK) {
                s_info.state = MIC_SESSION_STATE_ERROR;
                return err;
            }
            err = mic_bind_store(&record);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LINK_SESSION pair=responder persistence=failed err=%s",
                         esp_err_to_name(err));
                return err;
            }
            ESP_LOGI(TAG, "LINK_SESSION pair=responder result=ok channel=%u generation=%" PRIu32,
                     (unsigned)current_channel, record.generation);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGE(TAG, "LINK_SESSION pair=responder result=timeout");
    s_info.state = mic_session_ready() ? MIC_SESSION_STATE_READY : MIC_SESSION_STATE_UNBOUND;
    return ESP_ERR_TIMEOUT;
}

esp_err_t mic_session_unbind(void)
{
    // Releasing every key first is the caller's job at the key layer; here it is only
    // the link that must go quiet before the peer and record disappear.
    s_ready = false;
    s_info.paired = false;
    s_info.state = MIC_SESSION_STATE_UNBOUND;
    mic_radio_clear_peer(s_info.peer_mac);
    esp_err_t err = mic_bind_erase();
    ESP_LOGI(TAG, "LINK_SESSION unbind persistence=%s", err == ESP_OK ? "ok" : "failed");
    return err;
}
