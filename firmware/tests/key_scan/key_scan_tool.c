// Host-side test of the C3 key scanner: ladder classification plus debouncing.
//
// The cases mirror what the real ladder does and what it must never do. The measured
// plateaus (docs/hardware-c3.md section 4) are fed in as voltage readings so the windows
// themselves are exercised, not just the state machine.

#include <stdio.h>
#include <string.h>

#include "mic_key_scan.h"

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

static const mic_key_windows_t WINDOWS = {.up_max_mv = 150, .down_max_mv = 447, .ok_max_mv = 1900};
#define CONFIRM_MS 40
#define HOLD_MS 120

static void test_classification(void)
{
    printf("== classification ==\n");
    // Plateau centres from the measured distributions, plus the exact window boundaries.
    check(mic_key_classify(&WINDOWS, 3) == MIC_KEY_SLOT_UP, "raw UP plateau -> UP");
    check(mic_key_classify(&WINDOWS, 150) == MIC_KEY_SLOT_UP, "UP boundary is inclusive");
    check(mic_key_classify(&WINDOWS, 151) == MIC_KEY_SLOT_DOWN, "just above UP -> DOWN");
    check(mic_key_classify(&WINDOWS, 301) == MIC_KEY_SLOT_DOWN, "DOWN plateau -> DOWN");
    check(mic_key_classify(&WINDOWS, 447) == MIC_KEY_SLOT_DOWN, "DOWN boundary is inclusive");
    check(mic_key_classify(&WINDOWS, 448) == MIC_KEY_SLOT_OK, "just above DOWN -> OK");
    check(mic_key_classify(&WINDOWS, 597) == MIC_KEY_SLOT_OK, "OK plateau -> OK");
    check(mic_key_classify(&WINDOWS, 1900) == MIC_KEY_SLOT_OK, "OK boundary is inclusive");
    check(mic_key_classify(&WINDOWS, 1901) == MIC_KEY_SLOT_NONE, "just above OK -> released");
    check(mic_key_classify(&WINDOWS, 2918) == MIC_KEY_SLOT_NONE, "released plateau -> released");
    check(mic_key_classify(&WINDOWS, 4095) == MIC_KEY_SLOT_NONE, "rail -> released");
    check(mic_key_classify(&WINDOWS, -1) == MIC_KEY_SLOT_NONE, "negative reading -> released");
    check(mic_key_classify(NULL, 300) == MIC_KEY_SLOT_NONE, "null windows -> released");
}

static void test_idle_then_press(void)
{
    printf("== idle then press ==\n");
    mic_key_scan_t scan;
    mic_key_scan_init(&scan, &WINDOWS, CONFIRM_MS, HOLD_MS);
    mic_key_event_out_t events[2];

    // First reading establishes the baseline and emits nothing.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 0, events) == 0, "first idle emits nothing");

    // A press shorter than CONFIRM_MS is chatter and must not produce an event.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 10, events) == 0, "short press not yet accepted");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 20, events) == 0, "return to idle emits nothing");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 30, events) == 0, "second short press rejected");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 40, events) == 0, "return to idle again");

    // A sustained press is accepted once the confirm window elapses: exactly one event.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 50, events) == 0, "press starts the window");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 80, events) == 0, "still inside confirm window");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 90, events) == 1, "press accepted after 40 ms");
    check(events[0].key_id == MIC_KEY_SLOT_OK, "press reports OK");
    check(events[0].event == MIC_KEY_EVENT_PRESS, "press reports PRESS");

    // Holding it must not repeat: no event until the window changes.
    for (int t = 100; t < 400; t += 10) {
        check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, t, events) == 0, "held key does not repeat");
    }

    // The release needs HOLD_MS of a different window, because a held contact can drop
    // out briefly without the user letting go.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 400, events) == 0, "release starts hold window");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 500, events) == 0, "release inside hold window");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 520, events) == 1, "release accepted after 120 ms");
    check(events[0].key_id == MIC_KEY_SLOT_OK, "release reports OK");
    check(events[0].event == MIC_KEY_EVENT_RELEASE, "release reports RELEASE");

    // Idle again: no further events.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 600, events) == 0, "stays quiet once released");
}

static void test_chatter_while_held(void)
{
    printf("== chatter while a key is held ==\n");
    mic_key_scan_t scan;
    mic_key_scan_init(&scan, &WINDOWS, CONFIRM_MS, HOLD_MS);
    mic_key_event_out_t events[2];

    mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 0, events);
    mic_key_scan_feed(&scan, MIC_KEY_SLOT_UP, 10, events);
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_UP, 60, events) == 1, "UP accepted");

    // A 30 ms dropout while held is shorter than the 120 ms hold window: no event.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 70, events) == 0, "dropout starts");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_UP, 100, events) == 0, "contact returns before hold window");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_UP, 300, events) == 0, "still held, no repeat");

    // A dropout longer than the hold window IS a release.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 400, events) == 0, "long dropout starts");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 530, events) == 1, "long dropout is a release");
    check(events[0].event == MIC_KEY_EVENT_RELEASE, "long dropout reports RELEASE");
}

static void test_direct_window_move(void)
{
    printf("== direct window-to-window move ==\n");
    mic_key_scan_t scan;
    mic_key_scan_init(&scan, &WINDOWS, CONFIRM_MS, HOLD_MS);
    mic_key_event_out_t events[2];

    mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 0, events);
    mic_key_scan_feed(&scan, MIC_KEY_SLOT_UP, 10, events);
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_UP, 60, events) == 1, "UP accepted");

    // Sliding from UP to OK without passing through released: the ladder cannot really do
    // this, but a bouncing contact can. Both events must come out, release first, so the
    // receiver never holds two keys at once by accident.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 100, events) == 0, "OK starts the hold window");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 230, events) == 2, "window move emits two events");
    check(events[0].key_id == MIC_KEY_SLOT_UP && events[0].event == MIC_KEY_EVENT_RELEASE,
          "release of the old key comes first");
    check(events[1].key_id == MIC_KEY_SLOT_OK && events[1].event == MIC_KEY_EVENT_PRESS,
          "press of the new key comes second");
}

static void test_press_at_boot(void)
{
    printf("== key already held at boot ==\n");
    mic_key_scan_t scan;
    mic_key_scan_init(&scan, &WINDOWS, CONFIRM_MS, HOLD_MS);
    mic_key_event_out_t events[2];

    // The very first reading is a press. It is reported immediately rather than after a
    // confirm window, because there is no earlier state to debounce against.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_DOWN, 0, events) == 1, "held at boot reports PRESS");
    check(events[0].key_id == MIC_KEY_SLOT_DOWN, "held at boot reports DOWN");
    check(events[0].event == MIC_KEY_EVENT_PRESS, "held at boot reports PRESS");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_DOWN, 50, events) == 0, "no repeat");
}

static void test_reset(void)
{
    printf("== reset ==\n");
    mic_key_scan_t scan;
    mic_key_scan_init(&scan, &WINDOWS, CONFIRM_MS, HOLD_MS);
    mic_key_event_out_t events[2];

    mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 0, events);
    mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 10, events);
    mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 60, events);
    check(scan.held == MIC_KEY_SLOT_OK, "OK is held before reset");

    mic_key_scan_reset(&scan);
    check(scan.held == MIC_KEY_SLOT_NONE, "reset drops the held key");
    // The next reading re-establishes the baseline instead of reporting a release, and a
    // press after it has to earn the confirm window like any other transition.
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_NONE, 100, events) == 0, "reset then idle emits nothing");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_UP, 200, events) == 0, "press starts its window after reset");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_UP, 250, events) == 1, "scanning resumes after reset");
}

static void test_null_safety(void)
{
    printf("== null safety ==\n");
    mic_key_event_out_t events[2];
    mic_key_scan_t scan;
    mic_key_scan_init(&scan, &WINDOWS, CONFIRM_MS, HOLD_MS);
    check(mic_key_scan_feed(NULL, MIC_KEY_SLOT_OK, 0, events) == 0, "null scan is a no-op");
    check(mic_key_scan_feed(&scan, MIC_KEY_SLOT_OK, 0, NULL) == 0, "null out is a no-op");
    mic_key_scan_init(NULL, &WINDOWS, CONFIRM_MS, HOLD_MS);
    mic_key_scan_reset(NULL);
}

int main(void)
{
    printf("== key_scan host tests ==\n\n");
    test_classification();
    test_idle_then_press();
    test_chatter_while_held();
    test_direct_window_move();
    test_press_at_boot();
    test_reset();
    test_null_safety();

    printf("\n%d checks, %d failures\n", s_checks, s_failures);
    if (s_failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL\n");
    return 1;
}
