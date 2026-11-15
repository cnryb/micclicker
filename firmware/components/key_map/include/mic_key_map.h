#pragma once

// Runtime shortcut mapping for the three physical keys, and the key-state machine that
// turns mapped presses into standard 8-byte HID keyboard reports.
//
// This half of the mapping is deliberately free of NVS, ESP-IDF and TinyUSB: it takes a
// table, validates it, tracks which keys are down, and produces report bytes. That makes
// every rule AGENTS.md section 2 states about mapping ("非法映射不得写入", "修改映射前执行
// all-keys-up") checkable on the host in firmware/tests/key_map, and it keeps the panel
// that writes NVS (mic_key_store) from also owning the interpretation of a report.
//
// Defaults are the design baseline: UP = Left Alt, DOWN = reserved (no output),
// OK = Enter.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Three physical keys, ids fixed by docs/protocol.md (they are the wire key_id values).
#define MIC_KEY_COUNT 3

#define MIC_KEY_ID_UP 0x01
#define MIC_KEY_ID_DOWN 0x02
#define MIC_KEY_ID_OK 0x03

// Event values, matching the wire encoding. A press and a release are separate events,
// never a level: the receiver applies transitions, not a resampled state.
#define MIC_KEY_EVENT_RELEASE 0x00
#define MIC_KEY_EVENT_PRESS 0x01

// Standard HID keyboard report: modifiers, one reserved byte, then six key slots.
#define MIC_HID_KEYBOARD_REPORT_BYTES 8
#define MIC_HID_KEYBOARD_KEY_SLOTS 6

// Usage page 0x07 modifier bits, as carried in report byte 0.
#define MIC_HID_MOD_LEFT_CTRL 0x01
#define MIC_HID_MOD_LEFT_SHIFT 0x02
#define MIC_HID_MOD_LEFT_ALT 0x04
#define MIC_HID_MOD_LEFT_GUI 0x08
#define MIC_HID_MOD_RIGHT_CTRL 0x10
#define MIC_HID_MOD_RIGHT_SHIFT 0x20
#define MIC_HID_MOD_RIGHT_ALT 0x40
#define MIC_HID_MOD_RIGHT_GUI 0x80

// Common keycodes, spelled out so the defaults do not depend on a TinyUSB header.
#define MIC_HID_KEY_ENTER 0x28

// "No output at all" — the value DOWN carries by default.
#define MIC_HID_KEYCODE_NONE 0x00

typedef struct {
    uint8_t key_id;
    // Modifier bits to hold while this key is down. 0 means none.
    uint8_t modifier;
    // Keycode to place in the report, or MIC_HID_KEYCODE_NONE for "nothing".
    uint8_t keycode;
} mic_key_binding_t;

typedef struct {
    mic_key_binding_t bindings[MIC_KEY_COUNT];
} mic_key_map_t;

// Validate one binding without touching a table: usable as the gate a config tool call
// goes through before anything is persisted.
//
// Rejects an unknown key_id, a keycode in the reserved range 0x01..0x03 (the HID
// error/rollover usages, which a keyboard must never send deliberately) and anything
// above 0xE7, the last usage defined for page 0x07. A modifier byte is accepted as-is
// because all eight of its bits are defined.
esp_err_t mic_key_binding_validate(const mic_key_binding_t *binding);

// Validate a whole table: exactly one binding per key id, no duplicates, each binding
// individually valid. Returns ESP_ERR_INVALID_STATE when a table is only partly filled.
esp_err_t mic_key_map_validate(const mic_key_map_t *map);

// Fill in the design defaults (UP = Left Alt, DOWN = reserved, OK = Enter).
void mic_key_map_defaults(mic_key_map_t *map);

// Append the binding for `key_id`, or NULL when the table has no such key.
const mic_key_binding_t *mic_key_map_find(const mic_key_map_t *map, uint8_t key_id);

// Encode/decode the NVS blob. The layout is fixed-width little-endian with a version
// tag, so an older or corrupted blob is rejected instead of being reinterpreted.
#define MIC_KEY_MAP_BLOB_BYTES 16

void mic_key_map_encode(const mic_key_map_t *map, uint8_t out[MIC_KEY_MAP_BLOB_BYTES]);
esp_err_t mic_key_map_decode(mic_key_map_t *map, const uint8_t *blob, size_t len);

// Key-state machine. Holds the set of physical keys currently down and turns it into
// report bytes. Kept next to the map because the report is the map applied to state.
typedef struct {
    mic_key_map_t map;
    // Set of key ids currently held, as a bitmask over 1u << key_id.
    uint8_t down_mask;
} mic_key_state_t;

void mic_key_state_init(mic_key_state_t *state, const mic_key_map_t *map);

// Replace the table. The caller is responsible for having released every key first
// (AGENTS.md section 4: 修改映射前先 all-keys-up); this function still forces the held
// state to empty so a stale mask cannot outlive the table it referred to.
void mic_key_state_set_map(mic_key_state_t *state, const mic_key_map_t *map);

// Apply one debounced transition. Returns true when the held set actually changed, which
// is what tells the caller a new report is worth sending. A repeated press or release is
// absorbed here as well as on the link, because the link's duplicate filter is per
// sequence number and cannot see a same-state event resent under a new number.
bool mic_key_state_apply(mic_key_state_t *state, uint8_t key_id, uint8_t event);

// Force every key up. Returns true when anything was actually held.
bool mic_key_state_release_all(mic_key_state_t *state);

bool mic_key_state_any_down(const mic_key_state_t *state);

// Build the 8-byte report the current state implies. Modifiers are the union of every
// held binding's modifiers; keycodes fill the six slots in key-id order. Always writes
// all 8 bytes, so the all-zero report used for all-keys-up is just this call on an
// empty state.
void mic_key_state_report(const mic_key_state_t *state,
                          uint8_t report[MIC_HID_KEYBOARD_REPORT_BYTES]);

#ifdef __cplusplus
}
#endif
