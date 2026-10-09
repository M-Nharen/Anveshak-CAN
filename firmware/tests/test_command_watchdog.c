/* Disarms within 100 ms of the last command; disarms on term change. */
#include "minitest.h"
#include "test_helpers.h"

static void test_disarms_after_100ms_without_a_command(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);

    for (int i = 0; i < 4; i++) {
        t += 20u;
        acc_can_frame_t cmd =
            make_drive_cmd_frame(ROVER_NODE_DRIVE1, (uint8_t)(2 + i), ROVER_NODE_ORIN, 5u & 0x03u, 2u, 0, 0, 0u);
        test_rig_host_send(&rig, &cmd);
        test_rig_pump(&rig, t);
        anv_node_tick(&rig.node, t);
        MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);
    }

    t += ANV_CMD_WATCHDOG_MS + 1u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);
    MT_CHECK(rig.board_ctx.outputs_off);
}

static void test_disarms_on_term_change(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);

    /* RPi claims the lead with a higher term: every node disarms immediately. */
    acc_can_frame_t hb = make_host_hb_frame(ROVER_NODE_RPI, (uint8_t)ANV_HOST_ROLE_MASTER, 6u);
    test_rig_host_send(&rig, &hb);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);

    /* The old master's commands (stale term) no longer count as fresh. */
    acc_can_frame_t stale_cmd = make_drive_cmd_frame(ROVER_NODE_DRIVE1, 9u, ROVER_NODE_ORIN, 5u & 0x03u, 2u, 0, 0, 0u);
    test_rig_host_send(&rig, &stale_cmd);
    test_rig_pump(&rig, t);
    t += ANV_CMD_WATCHDOG_MS + 1u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);
}

static void test_frozen_sequence_counts_as_lost_master(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);

    /* Frames keep arriving (watchdog alone would stay happy) but the sequence counter is
     * stuck: this must still be treated as a lost master. */
    for (int i = 0; i < 6; i++) {
        t += 20u;
        acc_can_frame_t cmd = make_drive_cmd_frame(ROVER_NODE_DRIVE1, 1u, ROVER_NODE_ORIN, 5u & 0x03u, 2u, 0, 0, 0u);
        test_rig_host_send(&rig, &cmd);
        test_rig_pump(&rig, t);
        anv_node_tick(&rig.node, t);
    }
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);
}

int main(void)
{
    MT_RUN(test_disarms_after_100ms_without_a_command);
    MT_RUN(test_disarms_on_term_change);
    MT_RUN(test_frozen_sequence_counts_as_lost_master);
    return mt_summary();
}
