#include "anv_heartbeat.h"

#define ANV_RATE_SCHED_DISABLED_PERIOD_MS 0xFFFFFFFFu

void anv_rate_sched_init(anv_rate_sched_t *sched, uint32_t rate_hz, uint32_t now_ms)
{
    anv_rate_sched_set_rate(sched, rate_hz, now_ms);
}

void anv_rate_sched_set_rate(anv_rate_sched_t *sched, uint32_t rate_hz, uint32_t now_ms)
{
    sched->period_ms = (rate_hz == 0u) ? ANV_RATE_SCHED_DISABLED_PERIOD_MS : (1000u / rate_hz);
    sched->next_due_ms = now_ms + sched->period_ms;
}

bool anv_rate_sched_due(anv_rate_sched_t *sched, uint32_t now_ms)
{
    if (sched->period_ms == ANV_RATE_SCHED_DISABLED_PERIOD_MS) {
        return false;
    }
    if ((int32_t)(now_ms - sched->next_due_ms) < 0) {
        return false;
    }
    sched->next_due_ms = now_ms + sched->period_ms;
    return true;
}
