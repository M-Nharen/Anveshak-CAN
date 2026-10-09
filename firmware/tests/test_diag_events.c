/* NODE_EVENT: edge-triggered, cleared bit, one per code per second, sequence numbers. */
#include "minitest.h"
#include "test_diag_helpers.h"

#define NODE ROVER_NODE_DRIVE1

static void test_rate_limit_one_per_code_per_second(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    seen_event_t ev[SEEN_MAX];

    for (uint16_t i = 0; i < 5; i++) {
        anv_node_event(&rig.node, ROVER_ERR_BAD_FRAME, (uint16_t)(100 + i), t + i * 10u);
    }
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);
    MT_CHECK(ev[0].code == ROVER_ERR_BAD_FRAME && ev[0].detail == 100u);

    /* Nothing more before the second is over... */
    anv_node_tick(&rig.node, t + 900u);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 0u);

    /* ...then one frame carrying the latest detail. */
    anv_node_tick(&rig.node, t + 1000u);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);
    MT_CHECK(ev[0].detail == 104u);

    /* The log says how many were folded together (4 held back, the last one wins). */
    anv_log_entry_t e;
    MT_CHECK(anv_diag_log_get(&rig.node.diag, 0, &e));
    MT_CHECK(e.repeat == 4u);
}

static void test_rate_limit_is_per_code(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 2); /* info events are only on the bus at level 2 */
    seen_event_t ev[SEEN_MAX];

    anv_node_event(&rig.node, ROVER_ERR_BAD_FRAME, 1u, t);
    anv_node_event(&rig.node, ROVER_ERR_MASTER_CHANGED, 2u, t);
    anv_node_event(&rig.node, ROVER_ERR_CMD_FROM_NON_MASTER, 3u, t);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 3u);
}

static void test_edge_triggered_with_cleared_bit(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    seen_event_t ev[SEEN_MAX];

    anv_node_raise(&rig.node, ROVER_ERR_OVERCURRENT_A, 7u, t);
    anv_node_raise(&rig.node, ROVER_ERR_OVERCURRENT_A, 8u, t + 100u); /* still the same condition */
    anv_node_raise(&rig.node, ROVER_ERR_OVERCURRENT_A, 9u, t + 200u);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);
    MT_CHECK(!ev[0].cleared && ev[0].severity == ROVER_SEVERITY_ERROR && ev[0].detail == 7u);
    MT_CHECK(anv_diag_is_active(&rig.node.diag, ROVER_ERR_OVERCURRENT_A));

    /* Clearing after the limiter window goes out at once, with the cleared bit set. */
    anv_node_lower(&rig.node, ROVER_ERR_OVERCURRENT_A, 0u, t + 2000u);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);
    MT_CHECK(ev[0].cleared && ev[0].code == ROVER_ERR_OVERCURRENT_A);
    MT_CHECK(!anv_diag_is_active(&rig.node.diag, ROVER_ERR_OVERCURRENT_A));

    /* Clearing something that is not active sends nothing. */
    anv_node_lower(&rig.node, ROVER_ERR_OVERCURRENT_A, 0u, t + 4000u);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 0u);

    /* It can fire again after it was cleared. */
    anv_node_raise(&rig.node, ROVER_ERR_OVERCURRENT_A, 1u, t + 6000u);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);
}

static void test_clear_inside_rate_window_is_delayed_not_lost(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    seen_event_t ev[SEEN_MAX];

    anv_node_raise(&rig.node, ROVER_ERR_DRIVER_FAULT_PIN, 0u, t);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);
    anv_node_lower(&rig.node, ROVER_ERR_DRIVER_FAULT_PIN, 0u, t + 100u);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 0u);
    anv_node_tick(&rig.node, t + 1000u);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);
    MT_CHECK(ev[0].cleared);
}

static void test_sequence_numbers_count_only_sent_frames(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 0); /* level 0: warnings stay off the bus */
    seen_event_t ev[SEEN_MAX];

    anv_node_raise(&rig.node, ROVER_ERR_OVERCURRENT_A, 0u, t);          /* error: sent, seq 0 */
    anv_node_event(&rig.node, ROVER_ERR_BAD_FRAME, 0u, t);              /* warning: hidden */
    anv_node_raise(&rig.node, ROVER_ERR_OVERCURRENT_B, 0u, t);          /* error: sent, seq 1 */
    anv_node_event(&rig.node, ROVER_ERR_MASTER_CHANGED, 0u, t);         /* info: hidden */
    anv_node_raise(&rig.node, ROVER_ERR_DRIVER_FAULT_PIN, 0u, t);       /* error: sent, seq 2 */
    size_t n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 3u);
    MT_CHECK(ev[0].seq == 0u && ev[1].seq == 1u && ev[2].seq == 2u);

    /* The hidden ones are still in the black box, marked as not sent. */
    anv_log_entry_t e;
    MT_CHECK(anv_diag_log_count(&rig.node.diag) == 5u);
    MT_CHECK(anv_diag_log_get(&rig.node.diag, 1, &e));
    MT_CHECK(e.code == ROVER_ERR_MASTER_CHANGED && (e.flags & ANV_LOG_FLAG_NOT_SENT) != 0u);
}

static void test_full_tx_queue_retries_without_using_a_sequence_number(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    seen_event_t ev[SEEN_MAX];

    /* More distinct codes than the queue (8 per priority) holds, without draining. */
    const uint16_t codes[] = {ROVER_ERR_OVERCURRENT_A, ROVER_ERR_OVERCURRENT_B, ROVER_ERR_DRIVER_FAULT_PIN,
                              ROVER_ERR_UNDERVOLTAGE,  ROVER_ERR_OVERVOLTAGE,   ROVER_ERR_DRIVER_OVERTEMP,
                              ROVER_ERR_ENCODER_A_NO_COUNTS, ROVER_ERR_ENCODER_B_NO_COUNTS,
                              ROVER_ERR_ENCODER_DIRECTION, ROVER_ERR_PARAMS_CORRUPT};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        anv_node_raise(&rig.node, codes[i], 0u, t);
    }
    size_t first = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(first == 8u);
    anv_node_tick(&rig.node, t + 10u);
    size_t second = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(second == 2u);
    MT_CHECK(ev[0].seq == 8u && ev[1].seq == 9u); /* no gap */
}

static void test_command_timeout_event_and_cleared_on_next_command(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    seen_event_t ev[SEEN_MAX];
    (void)collect_events(&rig, ev, SEEN_MAX); /* MASTER_CHANGED from the handshake */

    t += 150u; /* past the 100 ms command watchdog */
    anv_node_tick(&rig.node, t);
    size_t n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u);
    MT_CHECK(ev[0].code == ROVER_ERR_CMD_TIMEOUT && !ev[0].cleared);
    MT_CHECK(anv_node_state(&rig.node) != ANV_STATE_ARMED);

    t += 2000u;
    acc_can_frame_t cmd = make_drive_cmd_frame(NODE, 2u, ROVER_NODE_ORIN, 1u, 2u, 0, 0, 0);
    test_rig_host_send(&rig, &cmd);
    test_rig_pump(&rig, t);
    n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u && ev[0].code == ROVER_ERR_CMD_TIMEOUT && ev[0].cleared);
}

static void test_master_and_non_master_events(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 2); /* info events are only on the bus at level 2 */
    seen_event_t ev[SEEN_MAX];

    /* The first master is not a change. */
    acc_can_frame_t hb = make_host_hb_frame(ROVER_NODE_ORIN, (uint8_t)ANV_HOST_ROLE_MASTER, 5u);
    test_rig_host_send(&rig, &hb);
    test_rig_pump(&rig, t);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 0u);

    /* The RPi takes over with a higher term. */
    hb = make_host_hb_frame(ROVER_NODE_RPI, (uint8_t)ANV_HOST_ROLE_MASTER, 6u);
    test_rig_host_send(&rig, &hb);
    test_rig_pump(&rig, t + 10u);
    size_t n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u && ev[0].code == ROVER_ERR_MASTER_CHANGED && ev[0].detail == ROVER_NODE_RPI);

    /* The Orin is no longer the master: its command is dropped and reported. */
    acc_can_frame_t cmd = make_drive_cmd_frame(NODE, 1u, ROVER_NODE_ORIN, 1u, 2u, 0, 0, 0);
    test_rig_host_send(&rig, &cmd);
    test_rig_pump(&rig, t + 20u);
    n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u && ev[0].code == ROVER_ERR_CMD_FROM_NON_MASTER && ev[0].detail == ROVER_NODE_ORIN);
}

static void test_bad_frame_and_estop_events(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    seen_event_t ev[SEEN_MAX];

    acc_can_frame_t cmd = make_drive_cmd_frame(NODE, 1u, ROVER_NODE_ORIN, 1u, 2u, 0, 0, 0);
    cmd.dlc = 3u; /* truncated */
    test_rig_host_send(&rig, &cmd);
    test_rig_pump(&rig, t);
    size_t n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u && ev[0].code == ROVER_ERR_BAD_FRAME && ev[0].detail == (cmd.id & 0x7FFu));

    acc_can_frame_t es = make_estop_frame();
    test_rig_host_send(&rig, &es);
    test_rig_pump(&rig, t + 10u);
    n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u && ev[0].code == ROVER_ERR_ESTOP_CAN && ev[0].severity == ROVER_SEVERITY_CRITICAL);
}

static void test_can_bus_events(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 2); /* info events are only on the bus at level 2 */
    seen_event_t ev[SEEN_MAX];

    rig.node_port.error_state.state = ACC_CAN_ERROR_PASSIVE;
    anv_node_tick(&rig.node, t + 10u);
    size_t n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u && ev[0].code == ROVER_ERR_CAN_ERROR_PASSIVE && !ev[0].cleared);

    rig.node_port.error_state.state = ACC_CAN_ERROR_ACTIVE;
    anv_node_tick(&rig.node, t + 2000u);
    n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u && ev[0].code == ROVER_ERR_CAN_ERROR_PASSIVE && ev[0].cleared);

    /* Bus-off: the event is queued, and once the driver recovers there is a RECOVERED event. */
    acc_can_loopback_force_bus_off(&rig.node_port);
    anv_node_tick(&rig.node, t + 4000u);
    anv_node_tick(&rig.node, t + 4010u);
    n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 2u);
    MT_CHECK(ev[0].code == ROVER_ERR_CAN_BUS_OFF && ev[0].severity == ROVER_SEVERITY_ERROR);
    MT_CHECK(ev[1].code == ROVER_ERR_CAN_BUS_OFF_RECOVERED);
}

static void test_ack_errors_become_a_warning_with_the_count(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    seen_event_t ev[SEEN_MAX];

    rig.node_port.error_state.error_type_counts[ACC_CAN_ERRTYPE_ACK] = 12u;
    anv_node_tick(&rig.node, t + 10u);
    size_t n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u && ev[0].code == ROVER_ERR_CAN_ACK_ERROR && ev[0].detail == 12u);
}

int main(void)
{
    MT_RUN(test_rate_limit_one_per_code_per_second);
    MT_RUN(test_rate_limit_is_per_code);
    MT_RUN(test_edge_triggered_with_cleared_bit);
    MT_RUN(test_clear_inside_rate_window_is_delayed_not_lost);
    MT_RUN(test_sequence_numbers_count_only_sent_frames);
    MT_RUN(test_full_tx_queue_retries_without_using_a_sequence_number);
    MT_RUN(test_command_timeout_event_and_cleared_on_next_command);
    MT_RUN(test_master_and_non_master_events);
    MT_RUN(test_bad_frame_and_estop_events);
    MT_RUN(test_can_bus_events);
    MT_RUN(test_ack_errors_become_a_warning_with_the_count);
    return mt_summary();
}
