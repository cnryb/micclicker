#include "mic_bind.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "mic_bind";

#define MIC_BIND_NAMESPACE "miclink"
#define MIC_BIND_KEY "binding"
// The record is stored with a version tag so a future field can be added without
// misreading an older blob as the new layout.
#define MIC_BIND_BLOB_VERSION 1u

typedef struct {
    uint32_t version;
    uint32_t generation;
    uint8_t peer_mac[ESP_NOW_ETH_ALEN];
    uint8_t channel;
    uint8_t lmk[ESP_NOW_KEY_LEN];
    uint8_t pmk[ESP_NOW_KEY_LEN];
} mic_bind_blob_t;

static bool mac_is_unicast(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    // A broadcast or multicast address cannot be a peer identity, and the group bit
    // in the first octet is the only reliable way to tell.
    return (mac[0] & 0x01) == 0;
}

esp_err_t mic_bind_validate(const mic_bind_record_t *record)
{
    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!record->paired) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!mac_is_unicast(record->peer_mac)) {
        return ESP_ERR_INVALID_ARG;
    }
    // An all-zero address is what an uninitialised or half-written record looks like.
    bool any_set = false;
    for (size_t i = 0; i < ESP_NOW_ETH_ALEN; ++i) {
        any_set = any_set || record->peer_mac[i] != 0;
    }
    if (!any_set) {
        return ESP_ERR_INVALID_ARG;
    }
    if (record->channel < 1 || record->channel > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t mic_bind_load(mic_bind_record_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MIC_BIND_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(read): %s", esp_err_to_name(err));
        return err;
    }

    mic_bind_blob_t blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, MIC_BIND_KEY, &blob, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob: %s", esp_err_to_name(err));
        return err;
    }
    if (size != sizeof(blob)) {
        // A size mismatch means the stored layout is not this one. Report it rather
        // than reinterpret bytes, and leave the record in place for inspection.
        ESP_LOGE(TAG, "Stored binding has %u bytes, expected %u", (unsigned)size,
                 (unsigned)sizeof(blob));
        return ESP_ERR_INVALID_SIZE;
    }
    if (blob.version != MIC_BIND_BLOB_VERSION) {
        ESP_LOGE(TAG, "Stored binding version %" PRIu32 ", expected %u", blob.version,
                 (unsigned)MIC_BIND_BLOB_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    out->paired = true;
    out->generation = blob.generation;
    memcpy(out->peer_mac, blob.peer_mac, ESP_NOW_ETH_ALEN);
    out->channel = blob.channel;
    memcpy(out->lmk, blob.lmk, ESP_NOW_KEY_LEN);
    memcpy(out->pmk, blob.pmk, ESP_NOW_KEY_LEN);

    err = mic_bind_validate(out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stored binding is invalid: %s", esp_err_to_name(err));
        memset(out, 0, sizeof(*out));
        return err;
    }
    return ESP_OK;
}

esp_err_t mic_bind_store(const mic_bind_record_t *record)
{
    esp_err_t err = mic_bind_validate(record);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Refusing to store an invalid binding: %s", esp_err_to_name(err));
        return err;
    }

    mic_bind_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = MIC_BIND_BLOB_VERSION;
    blob.generation = record->generation;
    memcpy(blob.peer_mac, record->peer_mac, ESP_NOW_ETH_ALEN);
    blob.channel = record->channel;
    memcpy(blob.lmk, record->lmk, ESP_NOW_KEY_LEN);
    memcpy(blob.pmk, record->pmk, ESP_NOW_KEY_LEN);

    nvs_handle_t handle;
    err = nvs_open(MIC_BIND_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(write): %s", esp_err_to_name(err));
        return err;
    }
    // One blob write is a single NVS entry update, so the record either becomes
    // fully visible or stays at its previous value after a brown-out.
    err = nvs_set_blob(handle, MIC_BIND_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Storing binding failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "BIND_STORED peer=" MACSTR " channel=%u generation=%" PRIu32, MAC2STR(record->peer_mac),
             (unsigned)record->channel, record->generation);
    return ESP_OK;
}

esp_err_t mic_bind_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MIC_BIND_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(erase): %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_erase_key(handle, MIC_BIND_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erasing binding failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "BIND_ERASED");
    return ESP_OK;
}
