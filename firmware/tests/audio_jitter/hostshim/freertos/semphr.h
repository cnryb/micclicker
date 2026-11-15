#pragma once

// Host-side stand-in for the FreeRTOS mutex the ring uses.
//
// mic_jitter.c guards its indices with a real FreeRTOS mutex because its producer runs
// in the Wi-Fi task and its consumer in the USB audio task: a spinlock there trips the
// interrupt watchdog. On the host the test drives the ring from one thread, so the
// mutex never contends and these are trivial. Only firmware/tests/audio_jitter puts
// this directory on the include path, so a firmware build resolves the real header.
#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

#define portMAX_DELAY ((uint32_t)0xffffffffu)

SemaphoreHandle_t host_sem_create_mutex(void);
int host_sem_take(SemaphoreHandle_t sem);
int host_sem_give(SemaphoreHandle_t sem);

#define xSemaphoreCreateMutex()                  host_sem_create_mutex()
#define xSemaphoreTake(sem, ticks)               host_sem_take(sem)
#define xSemaphoreGive(sem)                      host_sem_give(sem)
