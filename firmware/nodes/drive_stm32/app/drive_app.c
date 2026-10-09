#include "drive_app.h"

#include <string.h>

#include "ids.h"
#include "params_drive.h"
#include "rover.h"

static void refresh_params_snapshot(drive_app_t *app)
{
    anv_param_value_t v;
    rover_params_drive_t *p = &app->params_snapshot;

    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_PID_KP_A_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->pid_kp_a = v.f32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_PID_KI_A_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->pid_ki_a = v.f32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_PID_KD_A_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->pid_kd_a = v.f32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_PID_KP_B_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->pid_kp_b = v.f32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_PID_KI_B_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->pid_ki_b = v.f32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_PID_KD_B_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->pid_kd_b = v.f32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_CURRENT_LIMIT_MA_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->current_limit_ma = v.u32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_MAX_SPEED_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->max_speed = v.i32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_ENCODER_COUNTS_PER_REV_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->encoder_counts_per_rev = v.u32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_BRAKE_OR_COAST_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->brake_or_coast = (uint8_t)v.u32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_RATE_FB_HZ_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->rate_fb_hz = (uint8_t)v.u32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_RATE_TELEM_HZ_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->rate_telem_hz = (uint8_t)v.u32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_RATE_HB_HZ_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->rate_hb_hz = (uint8_t)v.u32;
    }
    if (anv_param_get(app->param_store, ROVER_PARAM_DRIVE_DEBUG_DEFAULT_INDEX, &v) == ANV_PARAM_STATUS_OK) {
        p->debug_default = (uint8_t)v.u32;
    }
}

void drive_app_build_param_descriptors(anv_param_descriptor_t *out)
{
    for (size_t i = 0; i < ROVER_PARAMS_DRIVE_TABLE_LEN; i++) {
        const rover_param_descriptor_t *src = &ROVER_PARAMS_DRIVE_TABLE[i];
        out[i].index = src->index;
        switch (src->type) {
        case ROVER_PARAM_TYPE_F32:
            out[i].type = ANV_PARAM_TYPE_F32;
            break;
        case ROVER_PARAM_TYPE_I32:
            out[i].type = ANV_PARAM_TYPE_I32;
            break;
        case ROVER_PARAM_TYPE_U32:
            out[i].type = ANV_PARAM_TYPE_U32;
            break;
        case ROVER_PARAM_TYPE_U8:
        default:
            out[i].type = ANV_PARAM_TYPE_U8;
            break;
        }
        out[i].minimum = src->minimum;
        out[i].maximum = src->maximum;
        out[i].default_value = src->default_value;
        out[i].disarmed_only = src->disarmed_only;
    }
}

void drive_app_init(drive_app_t *app, uint8_t node_id, const acc_can_driver_t *driver, acc_can_frame_t *txq_storage,
                     size_t txq_capacity_per_prio, anv_param_store_t *params, const anv_node_info_t *info,
                     const anv_board_hooks_t *board, const drive_motor_hw_t *motor_hw,
                     const drive_telem_hw_t *telem_hw, uint32_t now_ms)
{
    memset(app, 0, sizeof(*app));
    app->param_store = params;
    app->telem_hw = telem_hw;

    refresh_params_snapshot(app);
    drive_motor_app_init(&app->motor, motor_hw, &app->params_snapshot);

    anv_node_init(&app->node, node_id, DRIVE_NODE_TYPE, DRIVE_PROTOCOL_VERSION, driver, txq_storage,
                  txq_capacity_per_prio, params, info, board, now_ms);
    anv_node_boot_complete(&app->node);

    app->last_hb_rate_hz = app->params_snapshot.rate_hb_hz;
    anv_node_set_hb_rate(&app->node, app->last_hb_rate_hz, now_ms);

    app->last_telem_rate_hz = app->params_snapshot.rate_telem_hz;
    anv_rate_sched_init(&app->telem_sched, app->last_telem_rate_hz, now_ms);
}

static void send_drive_fb(drive_app_t *app)
{
    uint8_t fault_flags = 0u;
    if (app->motor.overcurrent[DRIVE_MOTOR_CHANNEL_A]) {
        fault_flags |= (1u << 0);
    }
    if (app->motor.overcurrent[DRIVE_MOTOR_CHANNEL_B]) {
        fault_flags |= (1u << 1);
    }
    if (anv_node_state(&app->node) == ANV_STATE_ESTOP) {
        fault_flags |= (1u << 7);
    }

    struct rover_drive1_fb_a_t fb_a = {
        .state = (uint8_t)anv_node_state(&app->node),
        .fault_flags = fault_flags,
        .speed_a = app->motor.measured_crad_s[DRIVE_MOTOR_CHANNEL_A],
        .speed_b = app->motor.measured_crad_s[DRIVE_MOTOR_CHANNEL_B],
        .current_a = (uint8_t)(app->motor.current_ma[DRIVE_MOTOR_CHANNEL_A] / 200u),
        .current_b = (uint8_t)(app->motor.current_ma[DRIVE_MOTOR_CHANNEL_B] / 200u),
    };
    acc_can_frame_t frame_a;
    frame_a.id = ROVER_CAN_ID(ROVER_CLASS_FEEDBACK, app->node.node_id, 0u);
    int packed = rover_drive1_fb_a_pack(frame_a.data, &fb_a, sizeof(frame_a.data));
    if (packed >= 0) {
        frame_a.dlc = (uint8_t)packed;
        (void)anv_node_send(&app->node, ACC_CAN_PRIO_FEEDBACK, &frame_a);
    }

    struct rover_drive1_fb_b_t fb_b = {
        .encoder_a = app->motor.last_encoder[DRIVE_MOTOR_CHANNEL_A],
        .encoder_b = app->motor.last_encoder[DRIVE_MOTOR_CHANNEL_B],
    };
    acc_can_frame_t frame_b;
    frame_b.id = ROVER_CAN_ID(ROVER_CLASS_FEEDBACK, app->node.node_id, 1u);
    packed = rover_drive1_fb_b_pack(frame_b.data, &fb_b, sizeof(frame_b.data));
    if (packed >= 0) {
        frame_b.dlc = (uint8_t)packed;
        (void)anv_node_send(&app->node, ACC_CAN_PRIO_FEEDBACK, &frame_b);
    }
}

void drive_app_on_rx_frame(drive_app_t *app, const acc_can_frame_t *frame, uint32_t now_ms)
{
    anv_node_on_rx_frame(&app->node, frame, now_ms);

    if (frame->id != ROVER_SYNC_FRAME_ID) {
        return;
    }

    /* "Every drive node applies the command it received during the previous cycle and
     * samples its encoders and currents" (docs/architecture.md, "The 20 ms control cycle").
     * anv_node_on_rx_frame() already moved the pending command into cmd_buf's active slot. */
    uint8_t len = 0;
    const uint8_t *active = anv_sync_buf_active(&app->node.cmd_buf, &len);
    if (active != NULL) {
        struct rover_drive1_cmd_t cmd;
        if (rover_drive1_cmd_unpack(&cmd, active, len) == 0) {
            drive_motor_app_on_cmd(&app->motor, cmd.mode, cmd.target_a, cmd.target_b, cmd.accel_limit);
        }
    }

    send_drive_fb(app);
}

void drive_app_motor_step(drive_app_t *app, uint32_t dt_ms)
{
    refresh_params_snapshot(app);
    bool armed = (anv_node_state(&app->node) == ANV_STATE_ARMED);
    drive_motor_app_step(&app->motor, armed, dt_ms);
}

static void send_telemetry(drive_app_t *app)
{
    struct rover_drive1_telem_0_t t0 = {
        .supply_voltage = app->telem_hw->read_supply_mv(app->telem_hw->ctx),
        .temp_driver_a = app->telem_hw->read_temp_driver_a(app->telem_hw->ctx),
        .temp_driver_b = app->telem_hw->read_temp_driver_b(app->telem_hw->ctx),
        .temp_mcu = app->telem_hw->read_temp_mcu(app->telem_hw->ctx),
        .tec = app->node.last_error_state.tec,
        .rec = app->node.last_error_state.rec,
        .bus_off_recovery_count =
            (uint8_t)(app->node.last_error_state.bus_off_count > 255u ? 255u : app->node.last_error_state.bus_off_count),
    };
    acc_can_frame_t frame0;
    frame0.id = ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, app->node.node_id, 0u);
    int packed = rover_drive1_telem_0_pack(frame0.data, &t0, sizeof(frame0.data));
    if (packed >= 0) {
        frame0.dlc = (uint8_t)packed;
        (void)anv_node_send(&app->node, ACC_CAN_PRIO_TELEMETRY, &frame0);
    }

    struct rover_drive1_telem_1_t t1 = {
        .duty_a = app->motor.duty_cpercent[DRIVE_MOTOR_CHANNEL_A],
        .duty_b = app->motor.duty_cpercent[DRIVE_MOTOR_CHANNEL_B],
        /* Not wired in this template: a real board needs a backup-register reset counter and
         * cause, and a DRIVE_CMD sequence-gap counter. Left at 0 -- see the PR description. */
        .watchdog_reset_count = 0u,
        .last_reset_cause = 0u,
        .commands_missed = 0u,
    };
    acc_can_frame_t frame1;
    frame1.id = ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, app->node.node_id, 1u);
    packed = rover_drive1_telem_1_pack(frame1.data, &t1, sizeof(frame1.data));
    if (packed >= 0) {
        frame1.dlc = (uint8_t)packed;
        (void)anv_node_send(&app->node, ACC_CAN_PRIO_TELEMETRY, &frame1);
    }
}

void drive_app_tick(drive_app_t *app, uint32_t now_ms)
{
    anv_node_tick(&app->node, now_ms);

    if (app->params_snapshot.rate_hb_hz != app->last_hb_rate_hz) {
        app->last_hb_rate_hz = app->params_snapshot.rate_hb_hz;
        anv_node_set_hb_rate(&app->node, app->last_hb_rate_hz, now_ms);
    }
    if (app->params_snapshot.rate_telem_hz != app->last_telem_rate_hz) {
        app->last_telem_rate_hz = app->params_snapshot.rate_telem_hz;
        anv_rate_sched_set_rate(&app->telem_sched, app->last_telem_rate_hz, now_ms);
    }

    if (anv_rate_sched_due(&app->telem_sched, now_ms)) {
        send_telemetry(app);
    }
}
