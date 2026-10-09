/* Generic rate scheduler: used for NODE_HB at its parameter rate, and reusable by the
 * application for telemetry scheduling from the same kind of rate parameter
 * (docs/architecture.md, "Heartbeat (NODE_HB at the rate parameter) and telemetry
 * scheduling from parameter rates"). */
#ifndef ANV_HEARTBEAT_H
#define ANV_HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t period_ms;
    uint32_t next_due_ms;
} anv_rate_sched_t;

/* rate_hz == 0 disables the schedule (anv_rate_sched_due() then never returns true). */
void anv_rate_sched_init(anv_rate_sched_t *sched, uint32_t rate_hz, uint32_t now_ms);
void anv_rate_sched_set_rate(anv_rate_sched_t *sched, uint32_t rate_hz, uint32_t now_ms);

/* True if the period has elapsed; advances the schedule from `now_ms` (not from the missed
 * deadline) so a late call does not cause a burst of back-to-back due reports. */
bool anv_rate_sched_due(anv_rate_sched_t *sched, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* ANV_HEARTBEAT_H */
