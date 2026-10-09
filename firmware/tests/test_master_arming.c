/* Arms only from the current master; ignores other hosts' commands. */
#include "minitest.h"
#include "test_helpers.h"

static void test_ignores_nmt_before_any_master_is_known(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);

    acc_can_frame_t nmt = make_nmt_frame(ROVER_NODE_DRIVE1, ANV_NMT_CMD_ARM, ROVER_NODE_RPI, 0);
    test_rig_host_send(&rig, &nmt);
    test_rig_pump(&rig, t);

    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);
}

static void test_arms_only_from_current_master(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);

    acc_can_frame_t hb = make_host_hb_frame(ROVER_NODE_ORIN, (uint8_t)ANV_HOST_ROLE_MASTER, 5u);
    test_rig_host_send(&rig, &hb);
    test_rig_pump(&rig, t);

    /* RPi is not the current master: its ARM must be ignored. */
    acc_can_frame_t bad_arm = make_nmt_frame(ROVER_NODE_DRIVE1, ANV_NMT_CMD_ARM, ROVER_NODE_RPI, 5u);
    test_rig_host_send(&rig, &bad_arm);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);

    /* Orin is the current master: its ARM is accepted. */
    acc_can_frame_t good_arm = make_nmt_frame(ROVER_NODE_DRIVE1, ANV_NMT_CMD_ARM, ROVER_NODE_ORIN, 5u);
    test_rig_host_send(&rig, &good_arm);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);
}

static void test_ignores_commands_from_non_master(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);

    /* RPi was never granted the lead; its DRIVE_CMD must not count as a fresh command. */
    acc_can_frame_t bad_cmd = make_drive_cmd_frame(ROVER_NODE_DRIVE1, 2u, ROVER_NODE_RPI, 5u & 0x03u, 2u, 100, 100, 50u);
    test_rig_host_send(&rig, &bad_cmd);
    test_rig_pump(&rig, t);

    t += ANV_CMD_WATCHDOG_MS + 1u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);
}

int main(void)
{
    MT_RUN(test_ignores_nmt_before_any_master_is_known);
    MT_RUN(test_arms_only_from_current_master);
    MT_RUN(test_ignores_commands_from_non_master);
    return mt_summary();
}
