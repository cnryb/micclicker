#include "mic_radio.h"

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

static const char *TAG = "mic_radio";

const uint8_t MIC_RADIO_BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// The send callback carries only the destination, so the in-flight frame length is
// tracked here to attribute bytes. Only one send is outstanding at a time in the
// probe firmware; if that ever changes, this must become a small queue.
static SemaphoreHandle_t s_stats_lock;
static mic_radio_stats_t s_stats;
static volatile size_t s_inflight_bytes;
static mic_radio_recv_fn s_recv_callback;
static void *s_recv_context;
static uint8_t s_local_mac[ESP_NOW_ETH_ALEN];
static uint8_t s_channel;

static void stats_lock(void)
{
    if (s_stats_lock != NULL) {
        xSemaphoreTake(s_stats_lock, portMAX_DELAY);
    }
}

static void stats_unlock(void)
{
    if (s_stats_lock != NULL) {
        xSemaphoreGive(s_stats_lock);
    }
}

static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    stats_lock();
    if (status == ESP_NOW_SEND_SUCCESS) {
        ++s_stats.tx_ok;
        s_stats.tx_bytes += (uint32_t)s_inflight_bytes;
    } else {
        ++s_stats.tx_fail;
    }
    stats_unlock();
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len <= 0) {
        return;
    }
    int8_t rssi = 0;
    if (info->rx_ctrl != NULL) {
        rssi = (int8_t)info->rx_ctrl->rssi;
    }

    stats_lock();
    ++s_stats.rx_frames;
    s_stats.rx_bytes += (uint32_t)len;
    // min_rssi starts at 0, which no 2.4 GHz reading reaches, so the first frame
    // always lowers it and the initial value needs no separate "primed" flag.
    if (s_stats.rx_frames == 1 || rssi < s_stats.min_rssi) {
        s_stats.min_rssi = rssi;
    }
    if (s_stats.rx_frames == 1 || rssi > s_stats.max_rssi) {
        s_stats.max_rssi = rssi;
    }
    s_stats.last_rssi = rssi;
    stats_unlock();

    if (s_recv_callback != NULL) {
        const uint8_t *src = info->src_addr;
        if (src != NULL) {
            s_recv_callback(src, data, len, rssi, s_recv_context);
        }
    }
}

esp_err_t mic_radio_start(const mic_radio_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->channel < 1 || config->channel > 13) {
        ESP_LOGE(TAG, "Rejected channel %u; only 1..13 are valid 2.4 GHz channels",
                 (unsigned)config->channel);
        return ESP_ERR_INVALID_ARG;
    }
    s_channel = config->channel;

    if (s_stats_lock == NULL) {
        s_stats_lock = xSemaphoreCreateMutex();
        if (s_stats_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // Never erase here: the NVS holds RF calibration and, on a paired unit, the
        // binding. A probe must fail visibly instead of wiping either.
        ESP_LOGE(TAG, "NVS init failed without erase: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return err;
    }

    // A previous stage may already have created the default STA netif.
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
            return ESP_FAIL;
        }
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        return err;
    }
    // Power save would gate the radio between DTIM beacons and add tens of
    // milliseconds of jitter to an unassociated link; the audio path cannot carry it.
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_ps: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    err = esp_wifi_get_channel(&primary, &second);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_channel: %s", esp_err_to_name(err));
        return err;
    }
    // Without an AP to join, the driver starts on a default channel. Moving to the
    // configured one here is what makes both ends hear each other.
    if (primary != s_channel) {
        err = esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_channel(%u): %s", (unsigned)s_channel, esp_err_to_name(err));
            return err;
        }
    }
    err = esp_wifi_get_channel(&primary, &second);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_channel after set: %s", esp_err_to_name(err));
        return err;
    }
    if (primary != s_channel) {
        ESP_LOGE(TAG, "Channel did not stick: wanted %u, got %u",
                 (unsigned)s_channel, (unsigned)primary);
        return ESP_FAIL;
    }

    err = esp_wifi_get_mac(WIFI_IF_STA, s_local_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_mac: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_now_register_send_cb(on_send);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_send_cb: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_now_register_recv_cb(on_recv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_recv_cb: %s", esp_err_to_name(err));
        return err;
    }

    // ESP-NOW refuses to transmit at all when the destination is not in the peer
    // list, and that includes broadcast: esp_now_send(NULL, ...) returns
    // ESP_ERR_ESPNOW_NOT_FOUND on an empty list rather than fanning out. Discovery
    // therefore needs the broadcast address registered as a peer before the first
    // send. It stays unencrypted, because ESP-NOW cannot encrypt broadcast.
    esp_now_peer_info_t broadcast;
    memset(&broadcast, 0, sizeof(broadcast));
    memcpy(broadcast.peer_addr, MIC_RADIO_BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    broadcast.channel = s_channel;
    broadcast.ifidx = WIFI_IF_STA;
    broadcast.encrypt = false;
    if (!esp_now_is_peer_exist(MIC_RADIO_BROADCAST_MAC)) {
        err = esp_now_add_peer(&broadcast);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Adding the broadcast peer failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    uint32_t version = 0;
    if (esp_now_get_version(&version) == ESP_OK) {
        ESP_LOGI(TAG, "ESP-NOW version=%" PRIu32, version);
    }
    ESP_LOGI(TAG, "RADIO_READY mac=" MACSTR " channel=%u encrypt_unicast=%d ps=none",
             MAC2STR(s_local_mac), (unsigned)s_channel, (int)config->encrypt_unicast);
    return ESP_OK;
}

esp_err_t mic_radio_set_receiver(mic_radio_recv_fn callback, void *context)
{
    s_recv_callback = callback;
    s_recv_context = context;
    return ESP_OK;
}

esp_err_t mic_radio_set_peer(const uint8_t mac[ESP_NOW_ETH_ALEN], bool encrypt,
                             const uint8_t lmk[ESP_NOW_KEY_LEN])
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (encrypt && lmk == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    if (encrypt) {
        memcpy(peer.lmk, lmk, ESP_NOW_KEY_LEN);
    }
    peer.channel = s_channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = encrypt;

    if (esp_now_is_peer_exist(mac)) {
        esp_err_t err = esp_now_mod_peer(&peer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_mod_peer(" MACSTR "): %s", MAC2STR(mac), esp_err_to_name(err));
        }
        return err;
    }
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_add_peer(" MACSTR ", encrypt=%d): %s", MAC2STR(mac), (int)encrypt,
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t mic_radio_clear_peer(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }
    return esp_now_del_peer(mac);
}

esp_err_t mic_radio_set_channel(uint8_t channel)
{
    if (channel < 1 || channel > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel == s_channel) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_channel(%u): %s", (unsigned)channel, esp_err_to_name(err));
        return err;
    }
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    err = esp_wifi_get_channel(&primary, &second);
    if (err != ESP_OK || primary != channel) {
        ESP_LOGE(TAG, "Channel %u did not stick (read back %u)", (unsigned)channel, (unsigned)primary);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    s_channel = channel;

    // Re-point every peer at the new channel; otherwise sends report ESP_ERR_ESPNOW_CHAN.
    esp_now_peer_num_t count = {0};
    if (esp_now_get_peer_num(&count) == ESP_OK) {
        esp_now_peer_info_t peer;
        bool from_head = true;
        while (esp_now_fetch_peer(from_head, &peer)) {
            from_head = false;
            if (peer.channel == channel) {
                continue;
            }
            peer.channel = channel;
            err = esp_now_mod_peer(&peer);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Re-pointing peer " MACSTR " to channel %u failed: %s", MAC2STR(peer.peer_addr),
                         (unsigned)channel, esp_err_to_name(err));
                return err;
            }
        }
    }
    ESP_LOGI(TAG, "Channel changed to %u", (unsigned)channel);
    return ESP_OK;
}

esp_err_t mic_radio_send(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > ESP_NOW_MAX_DATA_LEN) {
        // ESP-NOW v1.0 cannot carry more than 250 bytes, and silently truncating
        // would corrupt the frame the receiver then rejects.
        ESP_LOGE(TAG, "Refusing %u-byte send; ESP-NOW v1.0 limit is %u",
                 (unsigned)len, (unsigned)ESP_NOW_MAX_DATA_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    stats_lock();
    s_inflight_bytes = len;
    ++s_stats.tx_requests;
    stats_unlock();

    // Address broadcast explicitly instead of passing NULL. Both are documented to
    // broadcast, but NULL fans out to "every peer in the list", which on a list that
    // only holds the broadcast entry has produced ESP_ERR_ESPNOW_NOT_FOUND on this
    // ESP-IDF version. Sending to the broadcast address is the same transmission with
    // one unambiguous destination.
    const uint8_t *destination = mac != NULL ? mac : MIC_RADIO_BROADCAST_MAC;
    esp_err_t err = esp_now_send(destination, data, len);
    if (err != ESP_OK) {
        stats_lock();
        ++s_stats.tx_errors;
        stats_unlock();
    }
    return err;
}

esp_err_t mic_radio_set_pmk(const uint8_t pmk[ESP_NOW_KEY_LEN])
{
    if (pmk == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_now_set_pmk(pmk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_set_pmk: %s", esp_err_to_name(err));
    }
    return err;
}

void mic_radio_get_stats(mic_radio_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    stats_lock();
    *out = s_stats;
    stats_unlock();
}

void mic_radio_stats_delta(const mic_radio_stats_t *before, const mic_radio_stats_t *after,
                           mic_radio_stats_t *delta)
{
    if (before == NULL || after == NULL || delta == NULL) {
        return;
    }
    // Every counter is monotonic within a boot, so plain subtraction is correct.
    delta->tx_requests = after->tx_requests - before->tx_requests;
    delta->tx_ok = after->tx_ok - before->tx_ok;
    delta->tx_fail = after->tx_fail - before->tx_fail;
    delta->tx_errors = after->tx_errors - before->tx_errors;
    delta->tx_bytes = after->tx_bytes - before->tx_bytes;
    delta->rx_frames = after->rx_frames - before->rx_frames;
    delta->rx_bytes = after->rx_bytes - before->rx_bytes;
    // Signal quality describes the newest frame rather than a count, so an interval
    // takes the closing snapshot's values instead of a difference.
    delta->last_rssi = after->last_rssi;
    delta->min_rssi = after->min_rssi;
    delta->max_rssi = after->max_rssi;
}

const uint8_t *mic_radio_local_mac(void)
{
    return s_local_mac;
}

uint8_t mic_radio_channel(void)
{
    return s_channel;
}
