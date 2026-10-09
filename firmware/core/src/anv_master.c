#include "anv_master.h"

void anv_master_init(anv_master_tracker_t *tracker)
{
    tracker->have_master = false;
    tracker->master_node_id = 0;
    tracker->master_term = 0;
    tracker->last_master_hb_ms = 0;
}

bool anv_master_on_host_hb(anv_master_tracker_t *tracker, uint8_t sender_node_id, anv_host_role_t role,
                            uint16_t term, uint32_t now_ms)
{
    if (role != ANV_HOST_ROLE_MASTER) {
        return false;
    }

    tracker->last_master_hb_ms = now_ms;

    bool higher_or_equal = !tracker->have_master || term > tracker->master_term ||
                            (term == tracker->master_term && sender_node_id == tracker->master_node_id);
    if (!higher_or_equal) {
        return false;
    }

    bool identity_changed = tracker->have_master &&
                             (sender_node_id != tracker->master_node_id || term != tracker->master_term);

    tracker->master_node_id = sender_node_id;
    tracker->master_term = term;
    tracker->have_master = true;

    return identity_changed;
}

bool anv_master_check(const anv_master_tracker_t *tracker, uint8_t sender_node_id, uint8_t term_bits,
                       uint8_t term_mask)
{
    if (!tracker->have_master) {
        return false;
    }
    if (sender_node_id != tracker->master_node_id) {
        return false;
    }
    return ((uint8_t)(tracker->master_term & term_mask)) == (term_bits & term_mask);
}

bool anv_master_lost(const anv_master_tracker_t *tracker, uint32_t now_ms, uint32_t timeout_ms)
{
    if (!tracker->have_master) {
        return true;
    }
    return (now_ms - tracker->last_master_hb_ms) >= timeout_ms;
}

uint8_t anv_master_node_id(const anv_master_tracker_t *tracker)
{
    return tracker->master_node_id;
}

uint16_t anv_master_term(const anv_master_tracker_t *tracker)
{
    return tracker->master_term;
}
