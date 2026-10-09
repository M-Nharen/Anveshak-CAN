/* Bus-off: the node keeps outputs off and recovers through the driver hook. */
#include "minitest.h"
#include "test_helpers.h"

static void test_busoff_forces_outputs_off_and_recovers(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);
    rig.board_ctx.outputs_off = false; /* reset so we can tell the tick caused this */

    acc_can_loopback_force_bus_off(&rig.node_port);

    t += 10u;
    anv_node_tick(&rig.node, t);

    MT_CHECK(rig.board_ctx.outputs_off);

    acc_can_error_state_t err;
    MT_CHECK(rig.node_driver.get_error_state(rig.node_driver.ctx, &err) == ACC_CAN_OK);
    MT_CHECK(err.state == ACC_CAN_ERROR_ACTIVE); /* anv_node_tick already called start_recovery() */
    MT_CHECK(err.bus_off_count == 1u);

    /* While bus-off, the driver refuses to send. */
    acc_can_loopback_force_bus_off(&rig.node_port);
    acc_can_frame_t frame = make_sync_frame(0u);
    MT_CHECK(anv_node_send(&rig.node, ACC_CAN_PRIO_TELEMETRY, &frame));
    MT_CHECK(anv_node_drain_tx(&rig.node) == 0u);

    /* The next tick recovers it again. */
    t += 10u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(anv_node_drain_tx(&rig.node) == 1u);
}

int main(void)
{
    MT_RUN(test_busoff_forces_outputs_off_and_recovers);
    return mt_summary();
}
