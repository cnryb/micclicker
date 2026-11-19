// Host-side test of the mapping table and the HID report state machine.
//
// Every rule AGENTS.md section 2 states about the mapping is checked here rather than only
// on the board: the defaults, the rejection of an illegal table, and the fact that a
// report is a function of the held set and nothing else.

#include <stdio.h>
#include <string.h>

#include "mic_key_map.h"

static int s_checks;
static int s_failures;

static void check(int ok, const char *label)
{
    ++s_checks;
    if (!ok) {
        ++s_failures;
        printf("  FAIL %s\n", label);
    }
}

static void check_report(const mic_key_state_t *state, uint8_t modifier, const uint8_t *keys,
                         const char *label)
{
    uint8_t report[MIC_HID_KEYBOARD_REPORT_BYTES];
    mic_key_state_report(state, report);
    int ok = report[0] == modifier && report[1] == 0;
    for (size_t i = 0; i < MIC_HID_KEYBOARD_KEY_SLOTS; ++i) {
        uint8_t expected = keys == NULL ? 0 : keys[i];
        ok = ok && report[2 + i] == expected;
    }
    check(ok, label);
}

static void test_defaults(void)
{
    printf("== design defaults ==\n");
    mic_key_map_t map;
    mic_key_map_defaults(&map);
    check(mic_key_map_validate(&map) == ESP_OK, "the defaults are a valid table");

    const mic_key_binding_t *up = mic_key_map_find(&map, MIC_KEY_ID_UP);
    const mic_key_binding_t *down = mic_key_map_find(&map, MIC_KEY_ID_DOWN);
    const mic_key_binding_t *ok = mic_key_map_find(&map, MIC_KEY_ID_OK);
    check(up != NULL && up->modifier == MIC_HID_MOD_RIGHT_ALT && up->keycode == MIC_HID_KEYCODE_NONE,
          "UP defaults to Right Alt alone");
    check(down != NULL && down->modifier == MIC_HID_MOD_RIGHT_CTRL &&
              down->keycode == MIC_HID_KEYCODE_NONE,
          "DOWN defaults to Right Ctrl alone");
    check(ok != NULL && ok->modifier == 0 && ok->keycode == MIC_HID_KEY_ENTER,
          "OK defaults to Enter");
    check(mic_key_map_find(&map, 0x09) == NULL, "an unknown key id has no binding");
}

static void test_validation(void)
{
    printf("== validation ==\n");
    mic_key_binding_t binding = {.key_id = MIC_KEY_ID_UP, .modifier = 0, .keycode = 0x04};
    check(mic_key_binding_validate(&binding) == ESP_OK, "a plain letter is valid");

    // The three reserved usages are error codes, not keys: a mapping that names one would
    // make the host report an error on every press.
    for (uint8_t code = 0x01; code <= 0x03; ++code) {
        binding.keycode = code;
        check(mic_key_binding_validate(&binding) == ESP_ERR_INVALID_ARG,
              "reserved usage 0x01..0x03 is rejected");
    }
    binding.keycode = 0xE7;
    check(mic_key_binding_validate(&binding) == ESP_OK, "the last defined usage is accepted");
    binding.keycode = 0xE8;
    check(mic_key_binding_validate(&binding) == ESP_ERR_INVALID_ARG, "above the usage range is rejected");

    binding.keycode = MIC_HID_KEYCODE_NONE;
    check(mic_key_binding_validate(&binding) == ESP_OK, "no-output binding is valid");
    binding.modifier = MIC_HID_MOD_RIGHT_ALT;
    check(mic_key_binding_validate(&binding) == ESP_OK, "modifier-only binding is valid");

    binding.key_id = 0x09;
    check(mic_key_binding_validate(&binding) == ESP_ERR_INVALID_ARG, "unknown key id is rejected");
    check(mic_key_binding_validate(NULL) == ESP_ERR_INVALID_ARG, "null binding is rejected");

    // A table has to name every key exactly once; a duplicate would leave another key
    // silently unbound.
    mic_key_map_t map;
    mic_key_map_defaults(&map);
    check(mic_key_map_validate(&map) == ESP_OK, "complete table is valid");

    mic_key_map_t duplicate = map;
    duplicate.bindings[2].key_id = MIC_KEY_ID_UP;
    check(mic_key_map_validate(&duplicate) == ESP_ERR_INVALID_STATE, "duplicate key id is rejected");

    mic_key_map_t missing = map;
    missing.bindings[1].key_id = MIC_KEY_ID_UP;
    missing.bindings[2].key_id = MIC_KEY_ID_UP;
    check(mic_key_map_validate(&missing) == ESP_ERR_INVALID_STATE, "missing key id is rejected");

    mic_key_map_t bad = map;
    bad.bindings[2].keycode = 0x02;
    check(mic_key_map_validate(&bad) == ESP_ERR_INVALID_ARG, "an invalid binding fails the table");
    check(mic_key_map_validate(NULL) == ESP_ERR_INVALID_ARG, "null table is rejected");
}

static void test_blob_round_trip(void)
{
    printf("== NVS blob round trip ==\n");
    mic_key_map_t map;
    mic_key_map_defaults(&map);
    // A non-default table proves the encoder carries values rather than re-deriving them.
    map.bindings[1].modifier = MIC_HID_MOD_LEFT_SHIFT;
    map.bindings[1].keycode = 0x06;
    check(mic_key_map_validate(&map) == ESP_OK, "custom table is valid");

    uint8_t blob[MIC_KEY_MAP_BLOB_BYTES];
    mic_key_map_encode(&map, blob);
    check(blob[0] == 1 && blob[1] == 0 && blob[2] == 0 && blob[3] == 0, "blob starts with version 1");

    mic_key_map_t decoded;
    memset(&decoded, 0xFF, sizeof(decoded));
    check(mic_key_map_decode(&decoded, blob, sizeof(blob)) == ESP_OK, "blob decodes");
    check(memcmp(&decoded, &map, sizeof(map)) == 0, "decoded table equals the encoded one");

    check(mic_key_map_decode(&decoded, blob, sizeof(blob) - 1) == ESP_ERR_INVALID_SIZE,
          "a truncated blob is rejected");
    check(mic_key_map_decode(&decoded, blob, sizeof(blob) + 1) == ESP_ERR_INVALID_SIZE,
          "an oversized blob is rejected");

    uint8_t wrong_version[MIC_KEY_MAP_BLOB_BYTES];
    memcpy(wrong_version, blob, sizeof(blob));
    wrong_version[0] = 2;
    check(mic_key_map_decode(&decoded, wrong_version, sizeof(wrong_version)) == ESP_ERR_INVALID_VERSION,
          "an unknown version is rejected");

    // A blob whose values decode but fail validation must not be adopted: that is how a
    // corrupted record becomes a stuck key.
    uint8_t bad_code[MIC_KEY_MAP_BLOB_BYTES];
    memcpy(bad_code, blob, sizeof(blob));
    bad_code[4 + 4 + 1] = 0x02;  // DOWN keycode -> reserved usage
    check(mic_key_map_decode(&decoded, bad_code, sizeof(bad_code)) == ESP_ERR_INVALID_ARG,
          "a blob with an invalid binding is rejected");
    check(mic_key_map_decode(NULL, blob, sizeof(blob)) == ESP_ERR_INVALID_ARG, "null table is rejected");
    check(mic_key_map_decode(&decoded, NULL, sizeof(blob)) == ESP_ERR_INVALID_ARG, "null blob is rejected");
}

static void test_state_machine(void)
{
    printf("== held state and reports ==\n");
    mic_key_map_t map;
    mic_key_map_defaults(&map);
    mic_key_state_t state;
    mic_key_state_init(&state, &map);

    const uint8_t none[MIC_HID_KEYBOARD_KEY_SLOTS] = {0};
    check_report(&state, 0, none, "nothing held reports all-zero");
    check(!mic_key_state_any_down(&state), "nothing held");

    check(mic_key_state_apply(&state, MIC_KEY_ID_UP, MIC_KEY_EVENT_PRESS), "UP press changes state");
    check(mic_key_state_any_down(&state), "UP is held");
    check_report(&state, MIC_HID_MOD_RIGHT_ALT, none, "UP reports the Right Alt modifier");

    check(!mic_key_state_apply(&state, MIC_KEY_ID_UP, MIC_KEY_EVENT_PRESS), "repeat press is absorbed");
    check_report(&state, MIC_HID_MOD_RIGHT_ALT, none, "repeat press does not change the report");

    check(mic_key_state_apply(&state, MIC_KEY_ID_UP, MIC_KEY_EVENT_RELEASE), "UP release changes state");
    check_report(&state, 0, none, "release returns to all-zero");

    check(!mic_key_state_apply(&state, MIC_KEY_ID_UP, MIC_KEY_EVENT_RELEASE), "repeat release is absorbed");

    const uint8_t enter[MIC_HID_KEYBOARD_KEY_SLOTS] = {MIC_HID_KEY_ENTER, 0, 0, 0, 0, 0};
    check(mic_key_state_apply(&state, MIC_KEY_ID_OK, MIC_KEY_EVENT_PRESS), "OK press changes state");
    check_report(&state, 0, enter, "OK reports Enter in the first key slot");

    // DOWN defaults to Right Ctrl: holding it with OK ORs the modifier onto Enter.
    check(mic_key_state_apply(&state, MIC_KEY_ID_DOWN, MIC_KEY_EVENT_PRESS), "DOWN press changes state");
    check_report(&state, MIC_HID_MOD_RIGHT_CTRL, enter, "DOWN adds Right Ctrl to the report");
    check(mic_key_state_apply(&state, MIC_KEY_ID_DOWN, MIC_KEY_EVENT_RELEASE), "DOWN release changes state");
    check_report(&state, 0, enter, "releasing DOWN leaves Enter alone");
}

static void test_modifier_union(void)
{
    printf("== modifier union and slot ordering ==\n");
    mic_key_map_t map;
    mic_key_map_defaults(&map);
    // A table where both UP and OK carry modifiers, so holding both must OR them.
    map.bindings[0].modifier = MIC_HID_MOD_RIGHT_ALT;
    map.bindings[0].keycode = 0x04;
    map.bindings[2].modifier = MIC_HID_MOD_LEFT_SHIFT;
    map.bindings[2].keycode = 0x05;
    check(mic_key_map_validate(&map) == ESP_OK, "two-modifier table is valid");

    mic_key_state_t state;
    mic_key_state_init(&state, &map);
    mic_key_state_apply(&state, MIC_KEY_ID_UP, MIC_KEY_EVENT_PRESS);
    mic_key_state_apply(&state, MIC_KEY_ID_OK, MIC_KEY_EVENT_PRESS);

    uint8_t report[MIC_HID_KEYBOARD_REPORT_BYTES];
    mic_key_state_report(&state, report);
    check(report[0] == (MIC_HID_MOD_RIGHT_ALT | MIC_HID_MOD_LEFT_SHIFT),
          "held modifiers are OR'd into one byte");
    // Key-id order: UP (id 1) fills slot 0, OK (id 3) fills slot 1. Consistent ordering
    // matters because the host treats a reordered report as a new event.
    check(report[2] == 0x04 && report[3] == 0x05, "keycodes fill slots in key-id order");

    mic_key_state_apply(&state, MIC_KEY_ID_UP, MIC_KEY_EVENT_RELEASE);
    mic_key_state_report(&state, report);
    check(report[0] == MIC_HID_MOD_LEFT_SHIFT && report[2] == 0x05 && report[3] == 0,
          "releasing one key drops only its modifier and compacts the slots");
}

static void test_release_all(void)
{
    printf("== all-keys-up ==\n");
    mic_key_map_t map;
    mic_key_map_defaults(&map);
    mic_key_state_t state;
    mic_key_state_init(&state, &map);

    check(!mic_key_state_release_all(&state), "release-all on an empty state reports no change");

    mic_key_state_apply(&state, MIC_KEY_ID_UP, MIC_KEY_EVENT_PRESS);
    mic_key_state_apply(&state, MIC_KEY_ID_OK, MIC_KEY_EVENT_PRESS);
    check(mic_key_state_release_all(&state), "release-all reports a change while keys are held");
    check(!mic_key_state_any_down(&state), "nothing is held afterwards");

    const uint8_t none[MIC_HID_KEYBOARD_KEY_SLOTS] = {0};
    check_report(&state, 0, none, "the all-zero report is 8 zero bytes");
}

static void test_map_change_forces_release(void)
{
    printf("== changing the map releases held keys ==\n");
    mic_key_map_t map;
    mic_key_map_defaults(&map);
    mic_key_state_t state;
    mic_key_state_init(&state, &map);
    mic_key_state_apply(&state, MIC_KEY_ID_UP, MIC_KEY_EVENT_PRESS);

    mic_key_map_t other;
    mic_key_map_defaults(&other);
    other.bindings[0].keycode = 0x08;
    mic_key_state_set_map(&state, &other);
    // The held mask refers to keys whose meaning just changed, so it cannot survive the
    // swap; carrying it over would apply the old binding's release to the new table.
    check(!mic_key_state_any_down(&state), "a map change clears the held set");
}

static void test_null_safety(void)
{
    printf("== null safety ==\n");
    uint8_t report[MIC_HID_KEYBOARD_REPORT_BYTES];
    memset(report, 0xFF, sizeof(report));
    mic_key_state_report(NULL, report);
    for (size_t i = 0; i < sizeof(report); ++i) {
        check(report[i] == 0, "a null state reports all-zero");
        break;
    }
    check(!mic_key_state_apply(NULL, MIC_KEY_ID_UP, MIC_KEY_EVENT_PRESS), "null state apply is false");
    check(!mic_key_state_any_down(NULL), "null state holds nothing");
    mic_key_state_init(NULL, NULL);
    mic_key_state_set_map(NULL, NULL);
    mic_key_map_defaults(NULL);
    mic_key_map_encode(NULL, NULL);
}

int main(void)
{
    printf("== key_map host tests ==\n\n");
    test_defaults();
    test_validation();
    test_blob_round_trip();
    test_state_machine();
    test_modifier_union();
    test_release_all();
    test_map_change_forces_release();
    test_null_safety();

    printf("\n%d checks, %d failures\n", s_checks, s_failures);
    if (s_failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL\n");
    return 1;
}
