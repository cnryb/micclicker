// Host-side observer for the MicClicker keyboard interface.
//
// Stage 2 shipped a throw-away probe in .tmp that drove the keyboard through the config
// HID self-test commands. This is the acceptance counterpart: it never sends a command, it
// only watches the standard HID keyboard interface and reports what the host actually
// received. That distinction matters, because the key path being tested is
// "physical button -> C3 -> radio -> S3 -> HID report", and a command-driven test would
// start in the middle of it.
//
// It prints one machine-readable line per observed key transition so check.sh can grade a
// capture:
//
//   KEY_EVENT usage=226 value=1 elapsed_ms=1234
//   KEY_SUMMARY down=2 up=2 pairs=2 mods=0x04 keys=0x28
//
// Usage: ./key_observer <seconds> [--expect-pairs N] [--quiet]
// Exit: 0 when the run produced at least one complete pair (or the expected count), 1
// otherwise, 2 on usage or device errors.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_VID 0x303a
#define USB_PID 0x4014

static int s_down;
static int s_up;
static uint8_t s_mods_seen;
static uint8_t s_key_usages[16];
static int s_key_usage_count;
static bool s_quiet;
static int64_t s_start_ms;

static int64_t now_ms(void)
{
    return (int64_t)(CFAbsoluteTimeGetCurrent() * 1000.0);
}

static void note_usage(uint32_t usage)
{
    for (int i = 0; i < s_key_usage_count; ++i) {
        if (s_key_usages[i] == (uint8_t)usage) {
            return;
        }
    }
    if (s_key_usage_count < (int)sizeof(s_key_usages)) {
        s_key_usages[s_key_usage_count++] = (uint8_t)usage;
    }
}

static void key_event(void *context, IOReturn result, void *sender, IOHIDValueRef value)
{
    (void)context;
    (void)result;
    (void)sender;

    IOHIDElementRef element = IOHIDValueGetElement(value);
    uint32_t page = IOHIDElementGetUsagePage(element);
    if (page != 0x07) {
        return;  // Not the keyboard usage page.
    }
    uint32_t usage = IOHIDElementGetUsage(element);
    long state = (long)IOHIDValueGetIntegerValue(value);

    // Only count real keyboard usages. macOS also delivers ErrorRollOver (1-3), empty
    // keycode slots (0), and occasional UINT32_MAX phantoms on the same report change;
    // counting those as ups makes a clean run look unbalanced.
    bool is_modifier = (usage >= 0xE0 && usage <= 0xE7);
    bool is_keycode = (usage >= 0x04 && usage <= 0xDF);
    if (!is_modifier && !is_keycode) {
        return;
    }

    // The host reports each modifier bit and each keycode slot as its own element.
    if (is_modifier && state) {
        s_mods_seen |= (uint8_t)(1u << (usage - 0xE0));
    }

    if (state) {
        ++s_down;
        if (is_keycode) {
            note_usage(usage);
        }
    } else {
        ++s_up;
    }

    if (!s_quiet) {
        printf("KEY_EVENT page=0x%x usage=%u value=%ld elapsed_ms=%lld\n", page, usage, state,
               (long long)(now_ms() - s_start_ms));
        fflush(stdout);
    }
}

static int int_property(IOHIDDeviceRef device, CFStringRef key, int fallback)
{
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    if (value == NULL || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return fallback;
    }
    int result = fallback;
    CFNumberGetValue(value, kCFNumberIntType, &result);
    return result;
}

static IOHIDDeviceRef find_keyboard(IOHIDManagerRef manager)
{
    CFSetRef set = IOHIDManagerCopyDevices(manager);
    if (set == NULL) {
        return NULL;
    }
    CFIndex count = CFSetGetCount(set);
    const void **values = calloc((size_t)count, sizeof(void *));
    CFSetGetValues(set, values);
    IOHIDDeviceRef found = NULL;
    for (CFIndex i = 0; i < count; ++i) {
        IOHIDDeviceRef device = (IOHIDDeviceRef)values[i];
        int page = int_property(device, CFSTR(kIOHIDPrimaryUsagePageKey), -1);
        int usage = int_property(device, CFSTR(kIOHIDPrimaryUsageKey), -1);
        if (page == 1 && usage == 6) {
            found = (IOHIDDeviceRef)CFRetain(device);
            break;
        }
    }
    free(values);
    CFRelease(set);
    return found;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <seconds> [--expect-pairs N] [--quiet]\n", argv[0]);
        return 2;
    }
    double seconds = atof(argv[1]);
    int expect_pairs = -1;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--expect-pairs") == 0 && i + 1 < argc) {
            expect_pairs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--quiet") == 0) {
            s_quiet = true;
        } else {
            printf("KEY_OBSERVER: FAIL reason=unknown_arg arg=%s\n", argv[i]);
            return 2;
        }
    }
    if (seconds <= 0.0) {
        printf("KEY_OBSERVER: FAIL reason=bad_duration\n");
        return 2;
    }

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int vid = USB_VID;
    int pid = USB_PID;
    CFNumberRef vid_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef pid_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), vid_number);
    CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), pid_number);
    IOHIDManagerSetDeviceMatching(manager, match);

    if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        printf("KEY_OBSERVER: FAIL reason=manager_open\n");
        return 2;
    }

    IOHIDDeviceRef keyboard = find_keyboard(manager);
    if (keyboard == NULL) {
        printf("KEY_OBSERVER: FAIL reason=keyboard_not_found vid=0x%04x pid=0x%04x\n", USB_VID,
               USB_PID);
        return 2;
    }

    // Non-exclusive so the device keeps working for whatever else the desktop is doing.
    if (IOHIDDeviceOpen(keyboard, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        printf("KEY_OBSERVER: FAIL reason=device_open\n");
        return 2;
    }
    IOHIDDeviceRegisterInputValueCallback(keyboard, key_event, NULL);
    IOHIDDeviceScheduleWithRunLoop(keyboard, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    s_start_ms = now_ms();
    printf("KEY_OBSERVER: ARMED seconds=%.1f expect_pairs=%d\n", seconds, expect_pairs);
    fflush(stdout);

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);

    IOHIDDeviceUnscheduleFromRunLoop(keyboard, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDDeviceClose(keyboard, kIOHIDOptionsTypeNone);

    int pairs = s_down < s_up ? s_down : s_up;
    printf("KEY_SUMMARY down=%d up=%d pairs=%d mods=0x%02x keys=", s_down, s_up, pairs, s_mods_seen);
    for (int i = 0; i < s_key_usage_count; ++i) {
        printf("%s0x%02x", i == 0 ? "" : ",", s_key_usages[i]);
    }
    if (s_key_usage_count == 0) {
        printf("none");
    }
    printf("\n");

    int verdict = 1;
    if (expect_pairs > 0) {
        verdict = pairs >= expect_pairs ? 0 : 1;
    } else {
        verdict = pairs > 0 ? 0 : 1;
    }
    printf("KEY_OBSERVER: %s pairs=%d\n", verdict == 0 ? "PASS" : "FAIL", pairs);
    return verdict;
}
