/* 定时器实现，见定时器头文件里的语义说明。 */
#include "timer.h"

#include <stddef.h>

void fvp_timer_reset(FvpTimer *timers, int count)
{
    int i;

    if (timers == NULL || count <= 0) {
        return;
    }
    for (i = 0; i < count; ++i) {
        timers[i].active = 0;
        timers[i].start_ms = 0;
        timers[i].duration_ms = 0;
    }
}

int fvp_timer_set(FvpTimer *timers, int count, int id, int ms, uint64_t now_ms)
{
    if (timers == NULL || id < 0 || id >= count) {
        return 0;
    }
    if (ms < FVP_TIMER_MS_MIN) {
        ms = FVP_TIMER_MS_MIN;
    }
    if (ms > FVP_TIMER_MS_MAX) {
        ms = FVP_TIMER_MS_MAX;
    }
    timers[id].active = 1;
    timers[id].start_ms = now_ms;
    timers[id].duration_ms = ms;
    return 1;
}

uint64_t fvp_timer_elapsed(const FvpTimer *timers, int count, int id,
                           uint64_t now_ms)
{
    if (timers == NULL || id < 0 || id >= count || !timers[id].active) {
        return 0;
    }
    if (now_ms <= timers[id].start_ms) {
        return 0;
    }
    return now_ms - timers[id].start_ms;
}

int fvp_timer_get(const FvpTimer *timers, int count, int id, int opt,
                  uint64_t now_ms)
{
    uint64_t elapsed;

    if (timers == NULL || id < 0 || id >= count || !timers[id].active) {
        return 0;
    }
    elapsed = fvp_timer_elapsed(timers, count, id, now_ms);
    if (opt <= 0) {
        return (elapsed > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int)elapsed;
    }
    if (timers[id].duration_ms <= 0) {
        return 0;
    }
    /* elapsed*opt/duration：先按 64 位算，再夹到 int32 范围。 */
    {
        uint64_t v = (elapsed / (uint64_t)timers[id].duration_ms) * (uint64_t)opt;
        uint64_t rem = (elapsed % (uint64_t)timers[id].duration_ms) *
                       (uint64_t)opt / (uint64_t)timers[id].duration_ms;

        v += rem;
        if (v > 0x7FFFFFFFu) {
            v = 0x7FFFFFFFu;
        }
        return (int)v;
    }
}

int fvp_timer_active_count(const FvpTimer *timers, int count)
{
    int i;
    int n = 0;

    if (timers == NULL || count <= 0) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (timers[i].active) {
            n += 1;
        }
    }
    return n;
}