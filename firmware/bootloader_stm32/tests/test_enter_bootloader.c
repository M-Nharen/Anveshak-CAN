/* The application side of bootloader entry (core library): NMT ENTER_BOOTLOADER from the
 * master sets the stay flag and resets, but only while DISARMED. Lives with the bootloader
 * tests because it is half of the same hand-over. */
#include "minitest.h"
#include "test_helpers.h"

static void test_enter_bootloader_sets_flag_and_resets(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    acc_can_frame_t hb = make_host_hb_frame(ROVER_NODE_ORIN, (uint8_t)ANV_HOST_ROLE_MASTER, 5u);
    test_rig_host_send(&rig, &hb);
    test_rig_pump(&rig, t);

    acc_can_frame_t nmt = make_nmt_frame(ROVER_NODE_DRIVE1, ANV_NMT_CMD_ENTER_BOOTLOADER, ROVER_NODE_ORIN, 5u);
    test_rig_host_send(&rig, &nmt);
    test_rig_pump(&rig, t);

    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_BOOTLOADER);
    MT_CHECK(rig.board_ctx.enter_bootloader_calls == 1);
    MT_CHECK(rig.board_ctx.request_reset_calls == 1);
    MT_CHECK(rig.board_ctx.outputs_off);
}

static void test_enter_bootloader_refused_while_armed(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);

    acc_can_frame_t nmt = make_nmt_frame(ROVER_NODE_DRIVE1, ANV_NMT_CMD_ENTER_BOOTLOADER, ROVER_NODE_ORIN, 5u);
    test_rig_host_send(&rig, &nmt);
    test_rig_pump(&rig, t);

    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);
    MT_CHECK(rig.board_ctx.enter_bootloader_calls == 0);
    MT_CHECK(rig.board_ctx.request_reset_calls == 0);
}

static void test_enter_bootloader_from_non_master_ignored(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    acc_can_frame_t hb = make_host_hb_frame(ROVER_NODE_ORIN, (uint8_t)ANV_HOST_ROLE_MASTER, 5u);
    test_rig_host_send(&rig, &hb);
    test_rig_pump(&rig, t);

    acc_can_frame_t nmt = make_nmt_frame(ROVER_NODE_DRIVE1, ANV_NMT_CMD_ENTER_BOOTLOADER, ROVER_NODE_RPI, 5u);
    test_rig_host_send(&rig, &nmt);
    test_rig_pump(&rig, t);

    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);
    MT_CHECK(rig.board_ctx.enter_bootloader_calls == 0);
    MT_CHECK(rig.board_ctx.request_reset_calls == 0);
}

int main(void)
{
    MT_RUN(test_enter_bootloader_sets_flag_and_resets);
    MT_RUN(test_enter_bootloader_refused_while_armed);
    MT_RUN(test_enter_bootloader_from_non_master_ignored);
    return mt_summary();
}
