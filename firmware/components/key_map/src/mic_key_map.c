#include "mic_key_map.h"

#include <string.h>

// Highest usage defined for HID usage page 0x07 (Keyboard/Keypad).
#define KEYCODE_MAX 0xE7
// 0x01..0x03 are ErrorRollOver / POSTFail / ErrorUndefined. A keyboard must not send
// them deliberately, so a mapping that names one is a configuration error rather than a
// key choice.
#define KEYCODE_RESERVED_MIN 0x01
#define KEYCODE_RESERVED_MAX 0x03

#define BLOB_VERSION 1u

static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

esp_err_t mic_key_binding_validate(const mic_key_binding_t *binding)
{
    if (binding == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (binding->key_id != MIC_KEY_ID_UP && binding->key_id != MIC_KEY_ID_DOWN &&
        binding->key_id != MIC_KEY_ID_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (binding->keycode == MIC_HID_KEYCODE_NONE) {
        // "No output" is legal and is what DOWN carries by default. A modifier with no
        // keycode is still meaningful on its own (Right Alt alone), so it is allowed.
        return ESP_OK;
    }
    if (binding->keycode >= KEYCODE_RESERVED_MIN && binding->keycode <= KEYCODE_RESERVED_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (binding->keycode > KEYCODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t mic_key_map_validate(const mic_key_map_t *map)
{
    if (map == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // Every key id must appear exactly once. A partly filled table would leave a
    // physical key silently dead, which looks like a hardware fault at the desk.
    for (uint8_t id = MIC_KEY_ID_UP; id <= MIC_KEY_ID_OK; ++id) {
        size_t matches = 0;
        for (size_t i = 0; i < MIC_KEY_COUNT; ++i) {
            if (map->bindings[i].key_id == id) {
                ++matches;
            }
        }
        if (matches != 1) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    for (size_t i = 0; i < MIC_KEY_COUNT; ++i) {
        esp_err_t err = mic_key_binding_validate(&map->bindings[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

void mic_key_map_defaults(mic_key_map_t *map)
{
    if (map == NULL) {
        return;
    }
    memset(map, 0, sizeof(*map));
    map->bindings[0] = (mic_key_binding_t){.key_id = MIC_KEY_ID_UP,
                                           .modifier = MIC_HID_MOD_RIGHT_ALT,
                                           .keycode = MIC_HID_KEYCODE_NONE};
    map->bindings[1] = (mic_key_binding_t){.key_id = MIC_KEY_ID_DOWN,
                                           .modifier = 0,
                                           .keycode = MIC_HID_KEYCODE_NONE};
    map->bindings[2] = (mic_key_binding_t){.key_id = MIC_KEY_ID_OK,
                                           .modifier = 0,
                                           .keycode = MIC_HID_KEY_ENTER};
}

const mic_key_binding_t *mic_key_map_find(const mic_key_map_t *map, uint8_t key_id)
{
    if (map == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < MIC_KEY_COUNT; ++i) {
        if (map->bindings[i].key_id == key_id) {
            return &map->bindings[i];
        }
    }
    return NULL;
}

void mic_key_map_encode(const mic_key_map_t *map, uint8_t out[MIC_KEY_MAP_BLOB_BYTES])
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, MIC_KEY_MAP_BLOB_BYTES);
    if (map == NULL) {
        return;
    }
    put_u32_le(&out[0], BLOB_VERSION);
    // Positional, in key-id order, so the blob does not depend on table ordering.
    size_t offset = 4;
    for (uint8_t id = MIC_KEY_ID_UP; id <= MIC_KEY_ID_OK; ++id) {
        const mic_key_binding_t *binding = mic_key_map_find(map, id);
        if (binding != NULL) {
            out[offset] = binding->modifier;
            out[offset + 1] = binding->keycode;
        }
        offset += 4;
    }
}

esp_err_t mic_key_map_decode(mic_key_map_t *map, const uint8_t *blob, size_t len)
{
    if (map == NULL || blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len != MIC_KEY_MAP_BLOB_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (get_u32_le(&blob[0]) != BLOB_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    mic_key_map_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    size_t offset = 4;
    for (size_t i = 0; i < MIC_KEY_COUNT; ++i) {
        decoded.bindings[i].key_id = (uint8_t)(MIC_KEY_ID_UP + i);
        decoded.bindings[i].modifier = blob[offset];
        decoded.bindings[i].keycode = blob[offset + 1];
        offset += 4;
    }

    // A blob written by an older or corrupted version must not be adopted just because
    // it parsed: the same validation the config path uses applies here.
    esp_err_t err = mic_key_map_validate(&decoded);
    if (err != ESP_OK) {
        return err;
    }
    *map = decoded;
    return ESP_OK;
}

void mic_key_state_init(mic_key_state_t *state, const mic_key_map_t *map)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    if (map != NULL) {
        state->map = *map;
    }
}

void mic_key_state_set_map(mic_key_state_t *state, const mic_key_map_t *map)
{
    if (state == NULL || map == NULL) {
        return;
    }
    state->map = *map;
    // The mask refers to keys whose meaning just changed; keeping it would apply the
    // old binding's release to the new table.
    state->down_mask = 0;
}

bool mic_key_state_apply(mic_key_state_t *state, uint8_t key_id, uint8_t event)
{
    if (state == NULL) {
        return false;
    }
    if (key_id != MIC_KEY_ID_UP && key_id != MIC_KEY_ID_DOWN && key_id != MIC_KEY_ID_OK) {
        return false;
    }
    uint8_t bit = (uint8_t)(1u << key_id);
    bool was_down = (state->down_mask & bit) != 0;
    if (event == MIC_KEY_EVENT_PRESS) {
        if (was_down) {
            return false;
        }
        state->down_mask |= bit;
        return true;
    }
    if (!was_down) {
        return false;
    }
    state->down_mask &= (uint8_t)~bit;
    return true;
}

bool mic_key_state_release_all(mic_key_state_t *state)
{
    if (state == NULL || state->down_mask == 0) {
        return false;
    }
    state->down_mask = 0;
    return true;
}

bool mic_key_state_any_down(const mic_key_state_t *state)
{
    return state != NULL && state->down_mask != 0;
}

void mic_key_state_report(const mic_key_state_t *state,
                          uint8_t report[MIC_HID_KEYBOARD_REPORT_BYTES])
{
    if (report == NULL) {
        return;
    }
    memset(report, 0, MIC_HID_KEYBOARD_REPORT_BYTES);
    if (state == NULL || state->down_mask == 0) {
        return;
    }

    uint8_t modifiers = 0;
    size_t slot = 0;
    // Key-id order keeps the report stable for a given held set, which matters because
    // the host treats a changed report as a new event.
    for (uint8_t id = MIC_KEY_ID_UP; id <= MIC_KEY_ID_OK; ++id) {
        if ((state->down_mask & (uint8_t)(1u << id)) == 0) {
            continue;
        }
        const mic_key_binding_t *binding = mic_key_map_find(&state->map, id);
        if (binding == NULL) {
            continue;
        }
        modifiers |= binding->modifier;
        if (binding->keycode != MIC_HID_KEYCODE_NONE && slot < MIC_HID_KEYBOARD_KEY_SLOTS) {
            report[2 + slot] = binding->keycode;
            ++slot;
        }
    }
    report[0] = modifiers;
    report[1] = 0;
}
