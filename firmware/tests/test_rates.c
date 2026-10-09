/* Heartbeat rate: live changes, and the 5 Hz safety floor. (The drive app's feedback and
 * telemetry rates are tested in nodes/drive_stm32/tests.) */
#include "minitest.h"
#include "test_diag_helpers.h"

#define NODE ROVER_NODE_DRIVE1

static uint32_t count_heartbeats(test_rig_t *rig, uint32_t *t, uint32_t ms)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < ms; i++) {
        *t += 1u;
        anv_node_tick(&rig->node, *t);
        n += (uint32_t)count_frames_with_id(rig, ROVER_CAN_ID(ROVER_CLASS_HEARTBEAT, NODE, 0u));
    }
    return n;
}

static void test_floor_function(void)
{
    MT_CHECK(anv_rate_clamp_hb(0u) == ANV_MIN_HB_RATE_HZ);
    MT_CHECK(anv_rate_clamp_hb(1u) == 5u);
    MT_CHECK(anv_rate_clamp_hb(4u) == 5u);
    MT_CHECK(anv_rate_clamp_hb(5u) == 5u);
    MT_CHECK(anv_rate_clamp_hb(10u) == 10u);
}

static void test_heartbeat_rate_changes_live_but_not_below_the_floor(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);

    anv_node_set_hb_rate(&rig.node, 10u, t);
    uint32_t n = count_heartbeats(&rig, &t, 1000);
    MT_CHECK(n >= 9u && n <= 11u);

    anv_node_set_hb_rate(&rig.node, 20u, t);
    n = count_heartbeats(&rig, &t, 1000);
    MT_CHECK(n >= 19u && n <= 21u);

    anv_node_set_hb_rate(&rig.node, 1u, t); /* below the floor */
    n = count_heartbeats(&rig, &t, 2000);
    MT_CHECK(n >= 9u && n <= 11u); /* 5 Hz */

    anv_node_set_hb_rate(&rig.node, 0u, t); /* "off" is not allowed either */
    n = count_heartbeats(&rig, &t, 1000);
    MT_CHECK(n >= 4u && n <= 6u);
}

int main(void)
{
    MT_RUN(test_floor_function);
    MT_RUN(test_heartbeat_rate_changes_live_but_not_below_the_floor);
    return mt_summary();
}
