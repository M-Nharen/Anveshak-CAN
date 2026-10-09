#include "anv_estop.h"

void anv_estop_init(anv_estop_latch_t *estop)
{
    estop->latched = false;
}

void anv_estop_trigger(anv_estop_latch_t *estop)
{
    estop->latched = true;
}

void anv_estop_release(anv_estop_latch_t *estop)
{
    estop->latched = false;
}

bool anv_estop_is_latched(const anv_estop_latch_t *estop)
{
    return estop->latched;
}
