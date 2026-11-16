#ifndef MIC_LED_H
#define MIC_LED_H

#include <stdint.h>

// Board-level RGB LED.
//
// The ESP32-S3-N16R8 board carries a single-wire addressable LED (WS2812
// compatible) whose data input is GPIO48. It holds whatever colour it last
// latched, and after power-up with no frames on the wire it shows white, so
// the product firmware parks it at black as its first action and only lights
// it on request.
//
// A frame is bit-banged as RMT symbols: 24 bits of GRB plus a low reset gap.
// A "black" frame is 24 zero bits; the zero-bit high time must stay near
// 0.3 us, because a 0.7 us pulse is decoded as a one and would turn the LED
// white instead of off.

// Park the LED at black and start the BOOT-triggered indicator task. Call once,
// as early as possible after boot so the power-up white does not linger.
void mic_led_start(void);

// Set the LED colour. 0..255 per channel; (0, 0, 0) turns it off. Safe to call
// from a task; each call programs one frame on the LED data line.
void mic_led_set(uint8_t red, uint8_t green, uint8_t blue);

// Turn the LED off. Equivalent to mic_led_set(0, 0, 0).
void mic_led_off(void);

#endif
