/* ESTOP latch: set by the 0x000 frame (fixed payload 0xE5) from anyone, or a local E-stop
 * input; cleared only by ESTOP_RELEASE from the current master (checked by the caller before
 * calling anv_estop_release -- see anv_master_check). */
#ifndef ANV_ESTOP_H
#define ANV_ESTOP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_ESTOP_MAGIC ((uint8_t)0xE5u)

typedef struct {
    bool latched;
} anv_estop_latch_t;

void anv_estop_init(anv_estop_latch_t *estop);
void anv_estop_trigger(anv_estop_latch_t *estop);
void anv_estop_release(anv_estop_latch_t *estop);
bool anv_estop_is_latched(const anv_estop_latch_t *estop);

#ifdef __cplusplus
}
#endif

#endif /* ANV_ESTOP_H */
