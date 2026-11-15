#pragma once

// Debounced key scanning for the C3's single-pin ADC resistor ladder.
//
// Three buttons share one ADC pin and are told apart by the voltage window they pull
// the pin into (docs/hardware-c3.md section 4). Classification is kept separate from
// debouncing for two reasons: the windows are a board fact that belongs in board.h, and
// both halves then become testable on the host without an ADC
// (firmware/tests/key_scan).
//
// The state machine emits only debounced transitions, so the sender never has to guess
// whether a reading was chatter. A physical hold produces exactly one press, and the
// matching release comes when the window returns to released.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Voltage windows for the ladder. Boundaries are taken at the midpoints between the
// measured per-key plateaus; see board.h for the values and hardware-c3.md section 4
// for the measurement behind them.
typedef struct {
    int up_max_mv;    // <= this is UP
    int down_max_mv;  // <= this is DOWN
    int ok_max_mv;    // <= this is OK; anything above is "all released"
} mic_key_windows_t;

// Slot ids are the wire key ids from mic_link.h, so a scanning result needs no
// translation before it is put on the link. 0 means "nothing pressed".
#define MIC_KEY_SLOT_NONE 0x00
#define MIC_KEY_SLOT_UP 0x01
#define MIC_KEY_SLOT_DOWN 0x02
#define MIC_KEY_SLOT_OK 0x03

#define MIC_KEY_EVENT_RELEASE 0x00
#define MIC_KEY_EVENT_PRESS 0x01

typedef struct {
    uint8_t key_id;
    uint8_t event;
} mic_key_event_out_t;

// Map one voltage reading to a slot. Out-of-range values (negative, or above the OK
// window) all mean released, which is what the ladder does when no switch is closed.
uint8_t mic_key_classify(const mic_key_windows_t *windows, int millivolts);

typedef struct {
    mic_key_windows_t windows;
    // How long a new window must persist before it is accepted while no key is held.
    uint32_t confirm_ms;
    // How long a different window must persist while a key IS held. Longer than
    // confirm_ms because a held contact can drop out for a few milliseconds without the
    // user releasing it; treating that as a release would emit a spurious pair.
    uint32_t hold_ms;
    uint8_t held;  // debounced slot currently down, 0 when all keys are up
    uint8_t candidate;
    int64_t candidate_since_ms;
    bool initialized;
} mic_key_scan_t;

void mic_key_scan_init(mic_key_scan_t *scan, const mic_key_windows_t *windows,
                       uint32_t confirm_ms, uint32_t hold_ms);

// Drop the debounce history but keep the configuration. Used when the sampler restarts.
void mic_key_scan_reset(mic_key_scan_t *scan);

// Feed one classified slot reading taken at `now_ms` (a monotonic millisecond clock).
//
// Returns the number of debounced events appended to `out` (0, 1 or 2). Two events come
// out of a direct window-to-window move, which the ladder cannot produce electrically
// but which a bouncing contact can simulate; emitting the release before the press
// keeps the receiver's key state consistent instead of leaving the first key stuck.
size_t mic_key_scan_feed(mic_key_scan_t *scan, uint8_t slot, int64_t now_ms,
                         mic_key_event_out_t out[2]);

#ifdef __cplusplus
}
#endif
