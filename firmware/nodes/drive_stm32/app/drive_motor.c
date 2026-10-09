#include "drive_motor.h"

#include <string.h>

#define DRIVE_MOTOR_DUTY_MIN (-10000)
#define DRIVE_MOTOR_DUTY_MAX 10000

static int16_t clamp_i16(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) {
        return (int16_t)lo;
    }
    if (v > hi) {
        return (int16_t)hi;
    }
    return (int16_t)v;
}

static void reset_pid(drive_pid_channel_t *pid, float hold_at)
{
    pid->integral = 0.0f;
    pid->prev_measured = 0.0f;
    pid->ramped_target_crad_s = hold_at;
}

/* Encoder delta (counts) -> measured speed in 0.01 rad/s, DRIVE_FB_A's scale. */
static int16_t encoder_delta_to_crad_s(int32_t delta_counts, uint32_t dt_ms, uint32_t counts_per_rev)
{
    if (dt_ms == 0u || counts_per_rev == 0u) {
        return 0;
    }
    float revs = (float)delta_counts / (float)counts_per_rev;
    float rad = revs * 6.28318530717958647692f;
    float dt_s = (float)dt_ms / 1000.0f;
    float rad_per_s = rad / dt_s;
    return clamp_i16((int32_t)(rad_per_s * 100.0f), -32768, 32767);
}

/* Ramp `current` toward `target` at no more than `accel_crad_s2` (0.01 rad/s^2) per second
 * (docs/architecture.md: "Ramp toward the target at no more than the commanded acceleration
 * limit"). accel_crad_s2 == 0 means "no limit commanded": jump straight to the target. */
static void ramp_toward(float *current, int16_t target, uint16_t accel_crad_s2, uint32_t dt_ms)
{
    if (accel_crad_s2 == 0u) {
        *current = (float)target;
        return;
    }
    float max_step = (float)accel_crad_s2 * (float)dt_ms / 1000.0f;
    float error = (float)target - *current;
    if (error > max_step) {
        *current += max_step;
    } else if (error < -max_step) {
        *current -= max_step;
    } else {
        *current = (float)target;
    }
}

static int16_t pid_step(drive_pid_channel_t *pid, float target, float measured, const rover_params_drive_t *params,
                         bool channel_b, uint32_t dt_ms)
{
    float kp = channel_b ? params->pid_kp_b : params->pid_kp_a;
    float ki = channel_b ? params->pid_ki_b : params->pid_ki_a;
    float kd = channel_b ? params->pid_kd_b : params->pid_kd_a;
    float dt_s = (float)dt_ms / 1000.0f;

    float error = target - measured;
    float integral = pid->integral + error * dt_s;
    /* Derivative-on-measurement: a setpoint step does not itself spike the output. */
    float derivative = (dt_s > 0.0f) ? (measured - pid->prev_measured) / dt_s : 0.0f;

    float output = kp * error + ki * integral - kd * derivative;
    int16_t duty = clamp_i16((int32_t)output, DRIVE_MOTOR_DUTY_MIN, DRIVE_MOTOR_DUTY_MAX);

    /* Anti-windup: only keep integrating if we are not already saturated in the direction
     * that would make it worse. */
    if (duty != (int16_t)output || (duty == DRIVE_MOTOR_DUTY_MAX && error <= 0.0f) ||
        (duty == DRIVE_MOTOR_DUTY_MIN && error >= 0.0f)) {
        pid->integral = integral;
    }
    pid->prev_measured = measured;
    return duty;
}

void drive_motor_app_init(drive_motor_app_t *app, const drive_motor_hw_t *hw, const rover_params_drive_t *params)
{
    memset(app, 0, sizeof(*app));
    app->hw = hw;
    app->params = params;
    app->mode = DRIVE_MODE_COAST;
}

void drive_motor_app_on_cmd(drive_motor_app_t *app, uint8_t mode, int16_t target_a, int16_t target_b,
                            uint16_t accel_limit)
{
    int32_t max_speed = app->params->max_speed;
    app->mode = (drive_mode_t)mode;
    app->accel_limit_crad_s2 = accel_limit;

    if (app->mode == DRIVE_MODE_VELOCITY) {
        app->target_raw[DRIVE_MOTOR_CHANNEL_A] = clamp_i16(target_a, -max_speed, max_speed);
        app->target_raw[DRIVE_MOTOR_CHANNEL_B] = clamp_i16(target_b, -max_speed, max_speed);
    } else {
        /* DUTY mode (and coast/brake, which ignore the target): clamp to +-100%. */
        app->target_raw[DRIVE_MOTOR_CHANNEL_A] = clamp_i16(target_a, DRIVE_MOTOR_DUTY_MIN, DRIVE_MOTOR_DUTY_MAX);
        app->target_raw[DRIVE_MOTOR_CHANNEL_B] = clamp_i16(target_b, DRIVE_MOTOR_DUTY_MIN, DRIVE_MOTOR_DUTY_MAX);
    }
}

static void drive_safe_output(drive_motor_app_t *app, uint8_t ch)
{
    app->duty_cpercent[ch] = 0;
    reset_pid(&app->pid[ch], (float)app->measured_crad_s[ch]);
    if (app->params->brake_or_coast != 0u) {
        app->hw->set_brake(app->hw->ctx, ch);
    } else {
        app->hw->set_coast(app->hw->ctx, ch);
    }
}

void drive_motor_app_step(drive_motor_app_t *app, bool armed, uint32_t dt_ms)
{
    for (uint8_t ch = 0; ch < DRIVE_MOTOR_CHANNEL_COUNT; ch++) {
        int32_t encoder = app->hw->read_encoder(app->hw->ctx, ch);
        int32_t delta = encoder - app->last_encoder[ch];
        app->last_encoder[ch] = encoder;
        app->measured_crad_s[ch] = encoder_delta_to_crad_s(delta, dt_ms, app->params->encoder_counts_per_rev);

        app->current_ma[ch] = app->hw->read_current_ma(app->hw->ctx, ch);
        app->overcurrent[ch] = app->current_ma[ch] > app->params->current_limit_ma;

        if (!armed) {
            drive_safe_output(app, ch);
            continue;
        }

        bool channel_b = (ch == DRIVE_MOTOR_CHANNEL_B);
        switch (app->mode) {
        case DRIVE_MODE_COAST:
            app->duty_cpercent[ch] = 0;
            reset_pid(&app->pid[ch], (float)app->measured_crad_s[ch]);
            app->hw->set_coast(app->hw->ctx, ch);
            break;
        case DRIVE_MODE_BRAKE:
            app->duty_cpercent[ch] = 0;
            reset_pid(&app->pid[ch], (float)app->measured_crad_s[ch]);
            app->hw->set_brake(app->hw->ctx, ch);
            break;
        case DRIVE_MODE_DUTY: {
            int16_t duty = app->overcurrent[ch] ? 0 : app->target_raw[ch];
            app->duty_cpercent[ch] = duty;
            app->hw->set_duty(app->hw->ctx, ch, duty);
            break;
        }
        case DRIVE_MODE_VELOCITY:
        default: {
            ramp_toward(&app->pid[ch].ramped_target_crad_s, app->target_raw[ch], app->accel_limit_crad_s2, dt_ms);
            int16_t duty = pid_step(&app->pid[ch], app->pid[ch].ramped_target_crad_s,
                                     (float)app->measured_crad_s[ch], app->params, channel_b, dt_ms);
            if (app->overcurrent[ch]) {
                duty = 0;
            }
            app->duty_cpercent[ch] = duty;
            app->hw->set_duty(app->hw->ctx, ch, duty);
            break;
        }
        }
    }
}
