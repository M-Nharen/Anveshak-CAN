/* Host-side tests for drive_app.c on the loopback CAN bus (no hardware): checks that SYNC
 * drives DRIVE_FB_A/_B out, that a DRIVE_CMD only takes effect at the following SYNC, and
 * that telemetry is sent at the configured rate. Reuses test_helpers.h's frame builders and
 * fake board from firmware/tests, same as the core library's own tests. */
#include "minitest.h"
#include "test_helpers.h"

#include "drive_app.h"

typedef struct {
    int32_t encoder[DRIVE_MOTOR_CHANNEL_COUNT];
    int16_t duty[DRIVE_MOTOR_CHANNEL_COUNT];
} fake_motor_t;

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
    (void)ctx;
    (void)ch;
    return 0u;
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

int main(void)
{
    MT_RUN(test_sync_triggers_feedback_frames);
    MT_RUN(test_command_applies_only_at_next_sync);
    MT_RUN(test_telemetry_sent_at_configured_rate);
    return mt_summary();
}
