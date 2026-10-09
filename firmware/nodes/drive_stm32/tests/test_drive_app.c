/* Host-side tests for drive_app.c on the loopback CAN bus (no hardware): checks that SYNC
 * drives DRIVE_FB_A/_B out, that a DRIVE_CMD only takes effect at the following SYNC, and
 * that telemetry is sent at the configured rate. Reuses test_helpers.h's frame builders and
 * fake board from firmware/tests, same as the core library's own tests. */
#include "minitest.h"
#include "test_helpers.h"

#include "drive_app.h"
#include "errors.h"

typedef struct {
    int32_t encoder[DRIVE_MOTOR_CHANNEL_COUNT];
    int16_t duty[DRIVE_MOTOR_CHANNEL_COUNT];
    uint32_t current_ma[DRIVE_MOTOR_CHANNEL_COUNT];
} fake_motor_t;

static uint8_t g_reset_cause; /* what the fake board reports as the last reset */
static uint8_t fb_reset_cause(void *ctx)
{
    (void)ctx;
    return g_reset_cause;
}

static void fm_set_duty(void *ctx, uint8_t ch, int16_t duty)
{
    ((fake_motor_t *)ctx)->duty[ch] = duty;
}
static void fm_set_coast(void *ctx, uint8_t ch)
{
    ((fake_motor_t *)ctx)->duty[ch] = 0;
}
static void fm_set_brake(void *ctx, uint8_t ch)
{
    ((fake_motor_t *)ctx)->duty[ch] = 0;
}
static int32_t fm_read_encoder(void *ctx, uint8_t ch)
{
    fake_motor_t *m = (fake_motor_t *)ctx;
    m->encoder[ch] += m->duty[ch] / 100;
    return m->encoder[ch];
}
static uint32_t fm_read_current_ma(void *ctx, uint8_t ch)
{
    return ((fake_motor_t *)ctx)->current_ma[ch];
}

static uint16_t telem_supply_mv(void *ctx)
{
    (void)ctx;
    return 12000u;
}
static int8_t telem_temp(void *ctx)
{
    (void)ctx;
    return 25;
}

typedef struct {
    acc_can_loopback_bus_t bus;
    acc_can_loopback_port_t host_port;
    acc_can_loopback_port_t node_port;
    acc_can_driver_t host_driver;
    acc_can_driver_t node_driver;

    fake_board_ctx_t board_ctx;
    anv_board_hooks_t board;

    anv_param_descriptor_t descriptors[ROVER_PARAMS_DRIVE_TABLE_LEN];
    anv_param_value_t values[ROVER_PARAMS_DRIVE_TABLE_LEN];
    anv_param_store_t params;

    fake_motor_t motor;
    drive_motor_hw_t motor_hw;
    drive_telem_hw_t telem_hw;

    acc_can_frame_t txq_storage[ACC_CAN_PRIO_COUNT * 8u];
    drive_app_t app;
} drive_test_rig_t;

static void drive_rig_init(drive_test_rig_t *rig, uint32_t now_ms)
{
    memset(rig, 0, sizeof(*rig));

    acc_can_loopback_bus_init(&rig->bus);
    acc_can_loopback_attach(&rig->bus, &rig->host_port);
    acc_can_loopback_attach(&rig->bus, &rig->node_port);
    rig->host_driver = acc_can_loopback_make_driver(&rig->host_port);
    rig->node_driver = acc_can_loopback_make_driver(&rig->node_port);
    (void)rig->host_driver.init(rig->host_driver.ctx, 500000u, NULL, 0);
    (void)rig->node_driver.init(rig->node_driver.ctx, 500000u, NULL, 0);

    fake_board_init(&rig->board_ctx);
    rig->board = fake_board_hooks(&rig->board_ctx);
    rig->board.reset_cause = fb_reset_cause;

    build_drive_descriptors(rig->descriptors);
    anv_param_store_init(&rig->params, rig->values, rig->descriptors, ROVER_PARAMS_DRIVE_TABLE_LEN, &rig->board);
    anv_param_store_load(&rig->params);

    rig->motor_hw.ctx = &rig->motor;
    rig->motor_hw.set_duty = fm_set_duty;
    rig->motor_hw.set_coast = fm_set_coast;
    rig->motor_hw.set_brake = fm_set_brake;
    rig->motor_hw.read_encoder = fm_read_encoder;
    rig->motor_hw.read_current_ma = fm_read_current_ma;

    rig->telem_hw.ctx = NULL;
    rig->telem_hw.read_supply_mv = telem_supply_mv;
    rig->telem_hw.read_temp_driver_a = telem_temp;
    rig->telem_hw.read_temp_driver_b = telem_temp;
    rig->telem_hw.read_temp_mcu = telem_temp;

    anv_node_info_t info = {0};
    drive_app_init(&rig->app, ROVER_NODE_DRIVE1, &rig->node_driver, rig->txq_storage, 8u, &rig->params, &info,
                    &rig->board, &rig->motor_hw, &rig->telem_hw, now_ms);
}

static void drive_rig_pump(drive_test_rig_t *rig, uint32_t now_ms)
{
    acc_can_frame_t frame;
    while (rig->node_driver.receive(rig->node_driver.ctx, &frame)) {
        drive_app_on_rx_frame(&rig->app, &frame, now_ms);
    }
}

static bool drive_rig_host_recv(drive_test_rig_t *rig, acc_can_frame_t *frame)
{
    (void)anv_node_drain_tx(&rig->app.node);
    return rig->host_driver.receive(rig->host_driver.ctx, frame);
}

static void become_master_and_arm(drive_test_rig_t *rig, uint32_t now_ms)
{
    acc_can_frame_t hb = make_host_hb_frame(ROVER_NODE_ORIN, (uint8_t)ANV_HOST_ROLE_MASTER, 1u);
    (void)rig->host_driver.send(rig->host_driver.ctx, &hb, ACC_CAN_PRIO_SERVICE);
    drive_rig_pump(rig, now_ms);

    acc_can_frame_t cmd = make_drive_cmd_frame(ROVER_NODE_DRIVE1, 1u, ROVER_NODE_ORIN, 1u, (uint8_t)DRIVE_MODE_VELOCITY,
                                                0, 0, 0u);
    (void)rig->host_driver.send(rig->host_driver.ctx, &cmd, ACC_CAN_PRIO_SERVICE);
    drive_rig_pump(rig, now_ms);

    acc_can_frame_t nmt = make_nmt_frame(ROVER_NODE_DRIVE1, ANV_NMT_CMD_ARM, ROVER_NODE_ORIN, 1u);
    (void)rig->host_driver.send(rig->host_driver.ctx, &nmt, ACC_CAN_PRIO_SERVICE);
    drive_rig_pump(rig, now_ms);
}

static void test_sync_triggers_feedback_frames(void)
{
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);
    become_master_and_arm(&rig, t);
    MT_CHECK(anv_node_state(&rig.app.node) == ANV_STATE_ARMED);

    acc_can_frame_t sync = make_sync_frame(0u);
    (void)rig.host_driver.send(rig.host_driver.ctx, &sync, ACC_CAN_PRIO_SERVICE);
    drive_rig_pump(&rig, t);

    acc_can_frame_t frame;
    bool saw_fb_a = false, saw_fb_b = false;
    while (drive_rig_host_recv(&rig, &frame)) {
        if (frame.id == ROVER_CAN_ID(ROVER_CLASS_FEEDBACK, ROVER_NODE_DRIVE1, 0u)) {
            saw_fb_a = true;
        }
        if (frame.id == ROVER_CAN_ID(ROVER_CLASS_FEEDBACK, ROVER_NODE_DRIVE1, 1u)) {
            saw_fb_b = true;
        }
    }
    MT_CHECK(saw_fb_a);
    MT_CHECK(saw_fb_b);
}

static void test_command_applies_only_at_next_sync(void)
{
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);
    become_master_and_arm(&rig, t);

    acc_can_frame_t cmd = make_drive_cmd_frame(ROVER_NODE_DRIVE1, 2u, ROVER_NODE_ORIN, 1u, (uint8_t)DRIVE_MODE_DUTY,
                                                4000, -4000, 0u);
    (void)rig.host_driver.send(rig.host_driver.ctx, &cmd, ACC_CAN_PRIO_SERVICE);
    drive_rig_pump(&rig, t);

    MT_CHECK(rig.app.motor.mode != DRIVE_MODE_DUTY || rig.app.motor.target_raw[DRIVE_MOTOR_CHANNEL_A] != 4000);

    acc_can_frame_t sync = make_sync_frame(1u);
    (void)rig.host_driver.send(rig.host_driver.ctx, &sync, ACC_CAN_PRIO_SERVICE);
    drive_rig_pump(&rig, t);

    MT_CHECK(rig.app.motor.mode == DRIVE_MODE_DUTY);
    MT_CHECK(rig.app.motor.target_raw[DRIVE_MOTOR_CHANNEL_A] == 4000);
    MT_CHECK(rig.app.motor.target_raw[DRIVE_MOTOR_CHANNEL_B] == -4000);
}

static void test_telemetry_sent_at_configured_rate(void)
{
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);
    become_master_and_arm(&rig, t);
    /* debug_default is 0 (competition), which slows telemetry to 1 Hz; level 1 uses the parameter. */
    anv_debug_apply(&rig.app.node.debug, 1, ANV_DEBUG_CAT_ALL, 0, t);

    bool saw_telem0 = false;
    acc_can_frame_t frame;
    /* Default rate_telem_hz is 10 -> due every 100 ms; run for 300 ms of ticks. */
    for (uint32_t i = 0; i < 300; i++) {
        t += 1u;
        drive_app_tick(&rig.app, t);
        while (drive_rig_host_recv(&rig, &frame)) {
            if (frame.id == ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, ROVER_NODE_DRIVE1, 0u)) {
                saw_telem0 = true;
            }
        }
    }
    MT_CHECK(saw_telem0);
}

/* Runs `ms` milliseconds of 1 ms ticks and counts the frames the host sees with each of ids[]. */
static void run_ms(drive_test_rig_t *rig, uint32_t *t, uint32_t ms, const uint32_t *ids, size_t n_ids,
                   uint32_t *counts)
{
    acc_can_frame_t frame;
    for (uint32_t i = 0; i < ms; i++) {
        *t += 1u;
        drive_app_tick(&rig->app, *t);
        while (drive_rig_host_recv(rig, &frame)) {
            for (size_t k = 0; k < n_ids; k++) {
                if (frame.id == ids[k]) {
                    counts[k]++;
                }
            }
        }
    }
}

#define ID_TELEM0 ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, ROVER_NODE_DRIVE1, 0u)
#define ID_DEBUG_A ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, ROVER_NODE_DRIVE1, 2u)
#define ID_DEBUG_B ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, ROVER_NODE_DRIVE1, 3u)
#define ID_HB ROVER_CAN_ID(ROVER_CLASS_HEARTBEAT, ROVER_NODE_DRIVE1, 0u)
#define ID_EVENT ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, ROVER_NODE_DRIVE1, 7u)

static void test_level_0_slows_telemetry_to_1hz_and_level_1_restores_parameter_rate(void)
{
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);
    MT_CHECK(anv_debug_level(&rig.app.node.debug) == 0u); /* the debug_default parameter */

    const uint32_t ids[] = {ID_TELEM0};
    uint32_t counts[1] = {0};
    run_ms(&rig, &t, 3000, ids, 1, counts);
    MT_CHECK(counts[0] >= 2u && counts[0] <= 4u); /* ~1 Hz over 3 s */

    anv_debug_apply(&rig.app.node.debug, 1, ANV_DEBUG_CAT_ALL, 0, t);
    counts[0] = 0;
    run_ms(&rig, &t, 1000, ids, 1, counts);
    MT_CHECK(counts[0] >= 9u && counts[0] <= 11u); /* rate_telem_hz default 10 */
}

static void test_debug_pages_follow_the_level(void)
{
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);
    const uint32_t ids[] = {ID_DEBUG_A, ID_DEBUG_B};
    uint32_t counts[2];

    const struct {
        uint8_t level;
        uint32_t min, max;
    } cases[] = {{0, 0, 0}, {1, 0, 0}, {2, 9, 11}, {3, 48, 52}, {1, 0, 0}};
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anv_debug_apply(&rig.app.node.debug, cases[c].level, ANV_DEBUG_CAT_ALL, 0, t);
        counts[0] = counts[1] = 0;
        run_ms(&rig, &t, 1000, ids, 2, counts);
        MT_CHECK(counts[0] >= cases[c].min && counts[0] <= cases[c].max);
        MT_CHECK(counts[1] == counts[0]); /* A and B always go out together */
    }
}

static void test_debug_pages_carry_the_pid_state(void)
{
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);
    anv_debug_apply(&rig.app.node.debug, 2, ANV_DEBUG_CAT_ALL, 0, t);
    rig.app.motor.measured_crad_s[DRIVE_MOTOR_CHANNEL_A] = 123;
    rig.app.motor.duty_cpercent[DRIVE_MOTOR_CHANNEL_A] = -456;
    rig.app.motor.pid[DRIVE_MOTOR_CHANNEL_A].ramped_target_crad_s = 789.0f;
    rig.app.motor.pid[DRIVE_MOTOR_CHANNEL_A].integral = 40000.0f; /* clamps, doesn't wrap */

    acc_can_frame_t frame;
    bool seen = false;
    for (uint32_t i = 0; i < 200 && !seen; i++) {
        t += 1u;
        drive_app_tick(&rig.app, t);
        while (drive_rig_host_recv(&rig, &frame)) {
            struct rover_drive1_debug_a_t a;
            if (frame.id == ID_DEBUG_A && rover_drive1_debug_a_unpack(&a, frame.data, frame.dlc) == 0) {
                MT_CHECK(a.setpoint == 789 && a.measured == 123 && a.output == -456 && a.integrator == 32767);
                seen = true;
            }
        }
    }
    MT_CHECK(seen);
}

static void test_feedback_rate_divides_the_sync_cycle(void)
{
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);

    const uint32_t rates[] = {50u, 25u, 10u};
    const uint32_t expect_per_10_syncs[] = {10u, 5u, 2u};
    for (size_t r = 0; r < 3; r++) {
        MT_CHECK(anv_param_set(&rig.params, ROVER_PARAM_DRIVE_RATE_FB_HZ_INDEX, (anv_param_value_t){.u32 = rates[r]},
                               false) == ANV_PARAM_STATUS_OK);
        drive_app_motor_step(&rig.app, 1u); /* refreshes the live parameter snapshot */
        rig.app.sync_count = 0u;

        uint32_t fb_a = 0;
        acc_can_frame_t frame;
        for (uint8_t cycle = 0; cycle < 10; cycle++) {
            acc_can_frame_t sync = make_sync_frame(cycle);
            (void)rig.host_driver.send(rig.host_driver.ctx, &sync, ACC_CAN_PRIO_ESTOP);
            drive_rig_pump(&rig, t += 20u);
            while (drive_rig_host_recv(&rig, &frame)) {
                if (frame.id == ROVER_CAN_ID(ROVER_CLASS_FEEDBACK, ROVER_NODE_DRIVE1, 0u)) {
                    fb_a++;
                }
            }
        }
        MT_CHECK(fb_a == expect_per_10_syncs[r]);
    }
}

static void test_heartbeat_never_slower_than_5hz(void)
{
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);
    const uint32_t ids[] = {ID_HB};
    uint32_t counts[1] = {0};

    run_ms(&rig, &t, 1000, ids, 1, counts);
    MT_CHECK(counts[0] >= 9u && counts[0] <= 11u); /* default 10 Hz */

    /* rate_hb_hz = 1 is in the parameter's range, but the node still beats at 5 Hz. */
    MT_CHECK(anv_param_set(&rig.params, ROVER_PARAM_DRIVE_RATE_HB_HZ_INDEX, (anv_param_value_t){.u32 = 1u}, false) ==
             ANV_PARAM_STATUS_OK);
    drive_app_motor_step(&rig.app, 1u);
    counts[0] = 0;
    run_ms(&rig, &t, 2000, ids, 1, counts);
    MT_CHECK(counts[0] >= 9u && counts[0] <= 11u); /* 5 Hz over 2 s */

    /* A live change upward takes effect too. */
    MT_CHECK(anv_param_set(&rig.params, ROVER_PARAM_DRIVE_RATE_HB_HZ_INDEX, (anv_param_value_t){.u32 = 10u}, false) ==
             ANV_PARAM_STATUS_OK);
    drive_app_motor_step(&rig.app, 1u);
    counts[0] = 0;
    run_ms(&rig, &t, 1000, ids, 1, counts);
    MT_CHECK(counts[0] >= 9u && counts[0] <= 11u);
}

static void test_overcurrent_raises_and_clears_an_event(void)
{
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);
    become_master_and_arm(&rig, t);
    MT_CHECK(anv_node_state(&rig.app.node) == ANV_STATE_ARMED);

    rig.motor.current_ma[DRIVE_MOTOR_CHANNEL_A] = 60000u; /* far above current_limit_ma */
    drive_app_motor_step(&rig.app, 1u);
    drive_app_tick(&rig.app, t += 1u);

    bool raised = false;
    acc_can_frame_t frame;
    while (drive_rig_host_recv(&rig, &frame)) {
        struct rover_drive1_node_event_t ev;
        if (frame.id == ID_EVENT && rover_drive1_node_event_unpack(&ev, frame.data, frame.dlc) == 0 &&
            ev.error_code == ROVER_ERR_OVERCURRENT_A && !ev.cleared) {
            raised = true;
            MT_CHECK(ev.severity == ROVER_SEVERITY_ERROR && ev.detail == 60000u);
        }
    }
    MT_CHECK(raised);

    rig.motor.current_ma[DRIVE_MOTOR_CHANNEL_A] = 0u;
    t += 1500u;
    drive_app_motor_step(&rig.app, 1u);
    drive_app_tick(&rig.app, t);
    bool cleared = false;
    while (drive_rig_host_recv(&rig, &frame)) {
        struct rover_drive1_node_event_t ev;
        if (frame.id == ID_EVENT && rover_drive1_node_event_unpack(&ev, frame.data, frame.dlc) == 0 &&
            ev.error_code == ROVER_ERR_OVERCURRENT_A && ev.cleared) {
            cleared = true;
        }
    }
    MT_CHECK(cleared);
}

static void test_last_reset_cause_is_in_telemetry(void)
{
    g_reset_cause = ANV_RESET_BROWNOUT;
    drive_test_rig_t rig;
    uint32_t t = 1000;
    drive_rig_init(&rig, t);
    g_reset_cause = 0;

    bool seen = false;
    acc_can_frame_t frame;
    for (uint32_t i = 0; i < 1500 && !seen; i++) {
        t += 1u;
        drive_app_tick(&rig.app, t);
        while (drive_rig_host_recv(&rig, &frame)) {
            struct rover_drive1_telem_1_t t1;
            if (frame.id == ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, ROVER_NODE_DRIVE1, 1u) &&
                rover_drive1_telem_1_unpack(&t1, frame.data, frame.dlc) == 0) {
                MT_CHECK(t1.last_reset_cause == ANV_RESET_BROWNOUT);
                seen = true;
            }
        }
    }
    MT_CHECK(seen);
}

int main(void)
{
    MT_RUN(test_sync_triggers_feedback_frames);
    MT_RUN(test_command_applies_only_at_next_sync);
    MT_RUN(test_telemetry_sent_at_configured_rate);
    MT_RUN(test_level_0_slows_telemetry_to_1hz_and_level_1_restores_parameter_rate);
    MT_RUN(test_debug_pages_follow_the_level);
    MT_RUN(test_debug_pages_carry_the_pid_state);
    MT_RUN(test_feedback_rate_divides_the_sync_cycle);
    MT_RUN(test_heartbeat_never_slower_than_5hz);
    MT_RUN(test_overcurrent_raises_and_clears_an_event);
    MT_RUN(test_last_reset_cause_is_in_telemetry);
    return mt_summary();
}
