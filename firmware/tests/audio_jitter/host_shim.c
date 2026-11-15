// Host-only definition of the shimmed critical section declared in hostshim/FreeRTOS.h.
// It is compiled into the test binary rather than into the component, so the firmware
// build has exactly one implementation (the real FreeRTOS one).

#include <assert.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// A one-word standing mutex: the test is single-threaded, so "held" is enough state to
// catch a component bug that takes the lock twice or gives it while unheld.
static int s_held;

SemaphoreHandle_t host_sem_create_mutex(void)
{
    static int s_token;
    return &s_token;
}

int host_sem_take(SemaphoreHandle_t sem)
{
    assert(sem != NULL);
    // Re-entering would deadlock the real implementation; fail loudly here instead.
    assert(s_held == 0);
    s_held = 1;
    return pdTRUE;
}

int host_sem_give(SemaphoreHandle_t sem)
{
    assert(sem != NULL);
    assert(s_held == 1);
    s_held = 0;
    return pdTRUE;
}
