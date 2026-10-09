/* ESTOP latches; release + ARM needed to move. */
#include "minitest.h"
#include "test_helpers.h"

static void test_estop_latches_and_requires_release_then_arm(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);

    acc_can_frame_t estop = make_estop_frame();
    test_rig_host_send(&rig, &estop);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ESTOP);
    MT_CHECK(rig.board_ctx.outputs_off);

    acc_can_frame_t arm = make_nmt_frame(ROVER_NODE_DRIVE1, ANV_NMT_CMD_ARM, ROVER_NODE_ORIN, 5u);

    /* ARM alone cannot clear the latch. */
    test_rig_host_send(&rig, &arm);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ESTOP);

    /* A release from a host that is not the current master is ignored. */
    acc_can_frame_t bad_release = make_estop_release_frame(ROVER_NODE_RPI, 5u);
    test_rig_host_send(&rig, &bad_release);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ESTOP);

    /* Release from the current master clears the latch, but only to DISARMED. */
    acc_can_frame_t release = make_estop_release_frame(ROVER_NODE_ORIN, 5u);
    test_rig_host_send(&rig, &release);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);

    /* Only an explicit ARM moves it the rest of the way. */
    test_rig_host_send(&rig, &arm);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);
}

static void test_estop_overrides_from_any_state(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);

    acc_can_frame_t estop = make_estop_frame();
    test_rig_host_send(&rig, &estop);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ESTOP);
}

int main(void)
{
    MT_RUN(test_estop_latches_and_requires_release_then_arm);
    MT_RUN(test_estop_overrides_from_any_state);
    return mt_summary();
}
