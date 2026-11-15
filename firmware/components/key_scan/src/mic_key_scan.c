#include "mic_key_scan.h"

#include <string.h>

uint8_t mic_key_classify(const mic_key_windows_t *windows, int millivolts)
{
    if (windows == NULL) {
        return MIC_KEY_SLOT_NONE;
    }
    // Negative readings are impossible from a calibrated ADC but not from the raw
    // fallback path, and a negative value must not fall through to UP's window.
    if (millivolts < 0) {
        return MIC_KEY_SLOT_NONE;
    }
    if (millivolts <= windows->up_max_mv) {
        return MIC_KEY_SLOT_UP;
    }
    if (millivolts <= windows->down_max_mv) {
        return MIC_KEY_SLOT_DOWN;
    }
    if (millivolts <= windows->ok_max_mv) {
        return MIC_KEY_SLOT_OK;
    }
    return MIC_KEY_SLOT_NONE;
}

void mic_key_scan_init(mic_key_scan_t *scan, const mic_key_windows_t *windows,
                       uint32_t confirm_ms, uint32_t hold_ms)
{
    if (scan == NULL) {
        return;
    }
    memset(scan, 0, sizeof(*scan));
    if (windows != NULL) {
        scan->windows = *windows;
    }
    scan->confirm_ms = confirm_ms;
    scan->hold_ms = hold_ms;
}

void mic_key_scan_reset(mic_key_scan_t *scan)
{
    if (scan == NULL) {
        return;
    }
    scan->held = MIC_KEY_SLOT_NONE;
    scan->candidate = MIC_KEY_SLOT_NONE;
    scan->candidate_since_ms = 0;
    scan->initialized = false;
}

static uint32_t required_ms(const mic_key_scan_t *scan)
{
    return scan->held == MIC_KEY_SLOT_NONE ? scan->confirm_ms : scan->hold_ms;
}

size_t mic_key_scan_feed(mic_key_scan_t *scan, uint8_t slot, int64_t now_ms,
                         mic_key_event_out_t out[2])
{
    if (scan == NULL || out == NULL) {
        return 0;
    }

    // The first reading establishes the baseline instead of counting as a transition.
    // Without this the scanner would need confirm_ms of history before it could even
    // report "nothing is pressed", and a key already held at boot would be misread.
    if (!scan->initialized) {
        scan->initialized = true;
        scan->held = slot;
        scan->candidate = slot;
        scan->candidate_since_ms = now_ms;
        if (slot != MIC_KEY_SLOT_NONE) {
            out[0].key_id = slot;
            out[0].event = MIC_KEY_EVENT_PRESS;
            return 1;
        }
        return 0;
    }

    if (slot == scan->held) {
        // Back to the debounced state: any pending candidate is discarded, because the
        // window it represented did not survive long enough to be real.
        scan->candidate = slot;
        scan->candidate_since_ms = now_ms;
        return 0;
    }

    if (slot != scan->candidate) {
        scan->candidate = slot;
        scan->candidate_since_ms = now_ms;
        return 0;
    }

    if (now_ms - scan->candidate_since_ms < (int64_t)required_ms(scan)) {
        return 0;
    }

    size_t count = 0;
    uint8_t previous = scan->held;
    if (previous != MIC_KEY_SLOT_NONE) {
        out[count].key_id = previous;
        out[count].event = MIC_KEY_EVENT_RELEASE;
        ++count;
    }
    scan->held = MIC_KEY_SLOT_NONE;
    if (slot != MIC_KEY_SLOT_NONE) {
        out[count].key_id = slot;
        out[count].event = MIC_KEY_EVENT_PRESS;
        ++count;
        scan->held = slot;
    }
    // The transition itself restarts the timer, so chatter right after an accepted
    // transition has to earn the full window before it is believed.
    scan->candidate = slot;
    scan->candidate_since_ms = now_ms;
    return count;
}
