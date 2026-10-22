#include "hw.h"

#include <malloc.h>
#include <stdlib.h>
#include <xenon_soc/xenon_power.h>
#include <ppc/timebase.h>

#define HW_STACK_SIZE (1024 * 1024)

static uint8_t *stacks[6];

bool hw_start(int hw_thread, void (*fn)(void))
{
    if (hw_thread < 1 || hw_thread > 5)
        return false;
    if (stacks[hw_thread] == NULL) {
        stacks[hw_thread] = memalign(128, HW_STACK_SIZE);
        if (stacks[hw_thread] == NULL)
            return false;
    }
    hw_sync();
    /* Stack grows down; leave a red zone for the initial frame. */
    return xenon_run_thread_task(hw_thread, stacks[hw_thread] + HW_STACK_SIZE - 256, (void *)fn) == 0;
}

bool hw_running(int hw_thread)
{
    return xenon_is_thread_task_running(hw_thread) != 0;
}

uint64_t hw_now_us(void)
{
    return mftb() / (PPC_TIMEBASE_FREQ / 1000000);
}
