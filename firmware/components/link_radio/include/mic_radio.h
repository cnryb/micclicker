#pragma once

// ESP-NOW transport shared by both ends of the MicClicker link.
//
// This layer owns exactly three things: bringing Wi-Fi plus ESP-NOW up on a fixed
// 2.4 GHz channel without joining an AP, adding and removing peers (with or without
// encryption), and counting what the radio actually did. It deliberately does not
// interpret payloads — that is link_proto's job — and it never blocks inside the
// ESP-NOW callbacks, which run in the Wi-Fi task context.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

// DESIGN.md section 4.1 fixes the channel to the same value on both ends; 1 is the
// default because it is the only 2.4 GHz channel that does not overlap 6/11 and it
// sits outside the most common AP channel plans. The probe firmware overrides it.
#define MIC_RADIO_DEFAULT_CHANNEL 1

// Broadcast address, used only for discovery during a pairing window.
extern const uint8_t MIC_RADIO_BROADCAST_MAC[ESP_NOW_ETH_ALEN];

typedef struct {
    uint8_t channel;
    // Unicast traffic between bound peers is encrypted; discovery is not. See
    // docs/protocol.md for why the pairing handoff itself is plaintext.
    bool encrypt_unicast;
} mic_radio_config_t;

typedef struct {
    uint32_t tx_requests;      // calls to mic_radio_send that reached esp_now_send
    uint32_t tx_ok;            // send callbacks reporting success
    uint32_t tx_fail;          // send callbacks reporting failure
    uint32_t tx_errors;        // esp_now_send rejected the call immediately
    uint32_t tx_bytes;
    uint32_t rx_frames;
    uint32_t rx_bytes;
    int8_t last_rssi;
    int8_t min_rssi;
    int8_t max_rssi;
} mic_radio_stats_t;

// Called from the Wi-Fi task for every received frame. `mac` is the source address
// (never NULL). Implementations must copy anything they keep: the buffer is only
// valid for the duration of the call.
typedef void (*mic_radio_recv_fn)(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint8_t *data,
                                  int len, int8_t rssi, void *context);

// Bring up NVS (never erasing it), the network stack, Wi-Fi in STA mode on
// config->channel with power save disabled, then ESP-NOW itself.
esp_err_t mic_radio_start(const mic_radio_config_t *config);

// Register the single receive sink. Must be called after mic_radio_start succeeds.
esp_err_t mic_radio_set_receiver(mic_radio_recv_fn callback, void *context);

// Add or replace a peer. `lmk` must be 16 bytes and is used only when `encrypt` is
// true. Adding an existing peer falls back to esp_now_mod_peer so a re-pairing run
// does not need an explicit remove first.
esp_err_t mic_radio_set_peer(const uint8_t mac[ESP_NOW_ETH_ALEN], bool encrypt,
                             const uint8_t lmk[ESP_NOW_KEY_LEN]);

esp_err_t mic_radio_clear_peer(const uint8_t mac[ESP_NOW_ETH_ALEN]);

// Move to another 2.4 GHz channel and re-point an existing peer at it. Changing the
// channel while a peer still records the old one makes esp_now_send fail with
// ESP_ERR_ESPNOW_CHAN, so the two steps belong together. Used by the probe's channel
// sweep; the product firmware fixes the channel at pairing time and never moves.
esp_err_t mic_radio_set_channel(uint8_t channel);

// Send one frame. A NULL `mac` broadcasts to every configured peer.
//
// ESP-NOW copies the buffer before returning, so `data` may be a stack frame. The
// send result arrives later in the send callback, so a successful return here means
// "queued", not "delivered"; the counters keep the two separate.
esp_err_t mic_radio_send(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint8_t *data, size_t len);

// Set the primary master key. Must be called before any encrypted peer is added.
esp_err_t mic_radio_set_pmk(const uint8_t pmk[ESP_NOW_KEY_LEN]);

void mic_radio_get_stats(mic_radio_stats_t *out);

// Difference two snapshots taken with mic_radio_get_stats, so a probe phase can
// report its own figures without resetting counters another task is still writing.
// The caller keeps the "before" snapshot.
void mic_radio_stats_delta(const mic_radio_stats_t *before, const mic_radio_stats_t *after,
                           mic_radio_stats_t *delta);

const uint8_t *mic_radio_local_mac(void);

// Channel the link is currently on.
uint8_t mic_radio_channel(void);

#ifdef __cplusplus
}
#endif
