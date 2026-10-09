/* Parameter SET refused while armed for protected parameters. */
#include <string.h>

#include "minitest.h"
#include "test_helpers.h"

static void test_set_refused_while_armed_then_allowed_once_disarmed(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);

    float new_kp = 42.0f;
    uint32_t raw;
    memcpy(&raw, &new_kp, sizeof(raw));

    acc_can_frame_t set_req =
        make_service_set_frame(ROVER_NODE_DRIVE1, 0u /* Orin */, 7u, ROVER_PARAM_DRIVE_PID_KP_A_INDEX, raw);
    test_rig_host_send(&rig, &set_req);
    test_rig_pump(&rig, t);
    MT_CHECK(test_rig_drain(&rig) == 1u);

    acc_can_frame_t resp;
    MT_CHECK(rig.host_driver.receive(rig.host_driver.ctx, &resp));
    struct rover_drive1_service_resp_orin_t decoded;
    MT_CHECK(rover_drive1_service_resp_orin_unpack(&decoded, resp.data, resp.dlc) == 0);
    MT_CHECK(decoded.status == (uint8_t)ANV_PARAM_STATUS_NOT_ALLOWED_WHILE_ARMED);
    MT_CHECK(decoded.transaction_id == 7u);

    /* The stored value must be untouched. */
    anv_param_value_t v;
    MT_CHECK(anv_param_get(&rig.params, ROVER_PARAM_DRIVE_PID_KP_A_INDEX, &v) == ANV_PARAM_STATUS_OK);
    MT_CHECK(v.f32 != new_kp);

    acc_can_frame_t disarm = make_nmt_frame(ROVER_NODE_DRIVE1, ANV_NMT_CMD_DISARM, ROVER_NODE_ORIN, 5u);
    test_rig_host_send(&rig, &disarm);
    test_rig_pump(&rig, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);

    test_rig_host_send(&rig, &set_req);
    test_rig_pump(&rig, t);
    MT_CHECK(test_rig_drain(&rig) == 1u);
    MT_CHECK(rig.host_driver.receive(rig.host_driver.ctx, &resp));
    MT_CHECK(rover_drive1_service_resp_orin_unpack(&decoded, resp.data, resp.dlc) == 0);
    MT_CHECK(decoded.status == (uint8_t)ANV_PARAM_STATUS_OK);

    MT_CHECK(anv_param_get(&rig.params, ROVER_PARAM_DRIVE_PID_KP_A_INDEX, &v) == ANV_PARAM_STATUS_OK);
    MT_CHECK(v.f32 == new_kp);
}

static void test_set_out_of_range_rejected(void)
{
    test_rig_t rig;
    uint32_t t = 1000;
    test_rig_init(&rig, ROVER_NODE_DRIVE1, t);

    /* current_limit_ma's range is [0, 51000]; 999999 is well outside it. */
    uint32_t raw = 999999u;
    acc_can_frame_t set_req =
        make_service_set_frame(ROVER_NODE_DRIVE1, 0u, 1u, ROVER_PARAM_DRIVE_CURRENT_LIMIT_MA_INDEX, raw);
    test_rig_host_send(&rig, &set_req);
    test_rig_pump(&rig, t);
    MT_CHECK(test_rig_drain(&rig) == 1u);

    acc_can_frame_t resp;
    MT_CHECK(rig.host_driver.receive(rig.host_driver.ctx, &resp));
    struct rover_drive1_service_resp_orin_t decoded;
    MT_CHECK(rover_drive1_service_resp_orin_unpack(&decoded, resp.data, resp.dlc) == 0);
    MT_CHECK(decoded.status == (uint8_t)ANV_PARAM_STATUS_OUT_OF_RANGE);
}

int main(void)
{
    MT_RUN(test_set_refused_while_armed_then_allowed_once_disarmed);
    MT_RUN(test_set_out_of_range_rejected);
    return mt_summary();
}
