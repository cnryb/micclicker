#include "mic_key_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "key_store";

// A namespace of its own, so the binding record (link_bind, namespace "miclink") and the
// mapping cannot overwrite each other's blobs.
#define KEY_STORE_NAMESPACE "mickeys"
#define KEY_STORE_KEY "map"

esp_err_t mic_key_store_load(mic_key_map_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(KEY_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace absent: nothing has ever been saved on this unit.
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t blob[MIC_KEY_MAP_BLOB_BYTES];
    size_t length = sizeof(blob);
    err = nvs_get_blob(handle, KEY_STORE_KEY, blob, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length != MIC_KEY_MAP_BLOB_BYTES) {
        ESP_LOGW(TAG, "KEY_STORE: MAP size=%u expected=%u", (unsigned)length,
                 (unsigned)MIC_KEY_MAP_BLOB_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    return mic_key_map_decode(out, blob, length);
}

esp_err_t mic_key_store_save(const mic_key_map_t *map)
{
    esp_err_t err = mic_key_map_validate(map);
    if (err != ESP_OK) {
        // The caller sees the same rejection the decoder would produce, and NVS is
        // untouched: an invalid mapping must never reach storage (AGENTS.md section 2).
        return err;
    }

    uint8_t blob[MIC_KEY_MAP_BLOB_BYTES];
    mic_key_map_encode(map, blob);

    nvs_handle_t handle;
    err = nvs_open(KEY_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    // One blob, then commit: an interrupted write leaves the previous record, because the
    // new value only becomes visible after the commit.
    err = nvs_set_blob(handle, KEY_STORE_KEY, blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t mic_key_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KEY_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, KEY_STORE_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
