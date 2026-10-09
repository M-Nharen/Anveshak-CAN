/* Applies commands only at SYNC. */
#include "minitest.h"
#include "test_helpers.h"

static void test_commands_apply_only_at_sync(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);

    acc_can_frame_t hb = make_host_hb_frame(ROVER_NODE_ORIN, (uint8_t)ANV_HOST_ROLE_MASTER, 1u);
    test_rig_host_send(&rig, &hb);
    test_rig_pump(&rig, t);

    uint8_t len;
    MT_CHECK(anv_sync_buf_active(&rig.node.cmd_buf, &len) == NULL);

    acc_can_frame_t cmd = make_drive_cmd_frame(ROVER_NODE_DRIVE1, 1u, ROVER_NODE_ORIN, 1u, 2u, 500, -500, 100u);
    test_rig_host_send(&rig, &cmd);
    test_rig_pump(&rig, t);

    /* The command has been received, but not applied yet: no SYNC has happened. */
    MT_CHECK(anv_sync_buf_active(&rig.node.cmd_buf, &len) == NULL);

    acc_can_frame_t sync = make_sync_frame(0u);
    test_rig_host_send(&rig, &sync);
    test_rig_pump(&rig, t);

    const uint8_t *active = anv_sync_buf_active(&rig.node.cmd_buf, &len);
    MT_CHECK(active != NULL);
    struct rover_drive1_cmd_t decoded;
    MT_CHECK(rover_drive1_cmd_unpack(&decoded, active, len) == 0);
    MT_CHECK(decoded.target_a == 500);
    MT_CHECK(decoded.target_b == -500);

    /* A newer command arrives but SYNC has not happened again: the active (applied) command
     * must stay the one from the last SYNC. */
    acc_can_frame_t cmd2 = make_drive_cmd_frame(ROVER_NODE_DRIVE1, 2u, ROVER_NODE_ORIN, 1u, 2u, 999, 999, 100u);
    test_rig_host_send(&rig, &cmd2);
    test_rig_pump(&rig, t);

    active = anv_sync_buf_active(&rig.node.cmd_buf, &len);
    MT_CHECK(rover_drive1_cmd_unpack(&decoded, active, len) == 0);
    MT_CHECK(decoded.target_a == 500);

    /* The next SYNC picks up the newer command. */
    test_rig_host_send(&rig, &sync);
    test_rig_pump(&rig, t);
    active = anv_sync_buf_active(&rig.node.cmd_buf, &len);
    MT_CHECK(rover_drive1_cmd_unpack(&decoded, active, len) == 0);
    MT_CHECK(decoded.target_a == 999);
}

int main(void)
{
    MT_RUN(test_commands_apply_only_at_sync);
    return mt_summary();
}
