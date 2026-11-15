#pragma once

// Host-side stand-in for the FreeRTOS types the PCM ring uses.
//
// The ring's mutual exclusion lives in semphr.h; this header only has to supply the
// scalar types and pdTRUE/pdFALSE. Only firmware/tests/audio_jitter puts this directory
// on the include path, so a firmware build resolves the real FreeRTOS header instead,
// and the two must declare the same names.
#include <stdint.h>

// FreeRTOS return/priority types the component and its shims need.
#define pdTRUE  1
#define pdFALSE 0

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
