/* Host-side tests for drive_motor.c's ramp/PID/mode logic, against a trivial motor model (no
 * HAL, no hardware): duty maps linearly to an encoder rate, and current scales with |duty|.
 * The point is to check the software's behaviour (clamping, ramping, mode switching, the
 * armed gate, overcurrent cutoff), not to tune real PID gains against a real motor. */
#include "minitest.h"

#include <string.h>

#include "drive_motor.h"

typedef struct {
    int32_t encoder[DRIVE_MOTOR_CHANNEL_COUNT];
    float encoder_accum[DRIVE_MOTOR_CHANNEL_COUNT]; /* fractional counts carried between ticks */
    float velocity_crad_s[DRIVE_MOTOR_CHANNEL_COUNT]; /* the "real" plant's internal speed */
    int16_t duty[DRIVE_MOTOR_CHANNEL_COUNT];
    bool coast[DRIVE_MOTOR_CHANNEL_COUNT];
    bool brake[DRIVE_MOTOR_CHANNEL_COUNT];
    int set_duty_calls[DRIVE_MOTOR_CHANNEL_COUNT];
} fake_motor_t;

static void fm_init(fake_motor_t *m)
{
    memset(m, 0, sizeof(*m));
}

static void fm_set_duty(void *ctx, uint8_t ch, int16_t duty)
{
    fake_motor_t *m = (fake_motor_t *)ctx;
    m->duty[ch] = duty;
    m->coast[ch] = false;
    m->brake[ch] = false;
    m->set_duty_calls[ch]++;
}

static void fm_set_coast(void *ctx, uint8_t ch)
{
    fake_motor_t *m = (fake_motor_t *)ctx;
    m->duty[ch] = 0;
    m->coast[ch] = true;
    m->brake[ch] = false;
}

static void fm_set_brake(void *ctx, uint8_t ch)
{
    fake_motor_t *m = (fake_motor_t *)ctx;
    m->duty[ch] = 0;
    m->brake[ch] = true;
    m->coast[ch] = false;
}

/* A first-order motor: velocity eases toward a target proportional to duty with a ~20 ms time
 * constant (a little inertia, like a real motor+load), instead of jumping to it instantly.
 * That keeps a sane PID gain from bang-banging a noiseless, infinitely-stiff "plant". */
static int32_t fm_read_encoder(void *ctx, uint8_t ch)
{
    fake_motor_t *m = (fake_motor_t *)ctx;
    float target_crad_s = (float)m->duty[ch] * 1.5f;
    m->velocity_crad_s[ch] += (target_crad_s - m->velocity_crad_s[ch]) * (1.0f / 20.0f);

    float rad_per_s = m->velocity_crad_s[ch] / 100.0f;
    float counts_per_ms = rad_per_s * 4096.0f / 6.28318530718f * 0.001f;
    m->encoder_accum[ch] += counts_per_ms;
    int32_t whole_counts = (int32_t)m->encoder_accum[ch];
    m->encoder_accum[ch] -= (float)whole_counts;
    m->encoder[ch] += whole_counts;
    return m->encoder[ch];
}

static uint32_t fm_read_current_ma(void *ctx, uint8_t ch)
{
    fake_motor_t *m = (fake_motor_t *)ctx;
    int32_t d = m->duty[ch];
    if (d < 0) {
        d = -d;
    }
    return (uint32_t)(d * 2); /* 10000 (100%) -> 20000 mA */
}

static drive_motor_hw_t make_fake_hw(fake_motor_t *m)
{
    drive_motor_hw_t hw;
    hw.ctx = m;
    hw.set_duty = fm_set_duty;
    hw.set_coast = fm_set_coast;
    hw.set_brake = fm_set_brake;
    hw.read_encoder = fm_read_encoder;
    hw.read_current_ma = fm_read_current_ma;
    return hw;
}

static rover_params_drive_t make_test_params(void)
{
    rover_params_drive_t p = ROVER_PARAMS_DRIVE_DEFAULTS;
    return p;
}

static void test_coast_mode_outputs_nothing(void)
{
    fake_motor_t motor;
    fm_init(&motor);
    drive_motor_hw_t hw = make_fake_hw(&motor);
    rover_params_drive_t params = make_test_params();
    drive_motor_app_t app;
    drive_motor_app_init(&app, &hw, &params);

    drive_motor_app_on_cmd(&app, (uint8_t)DRIVE_MODE_COAST, 1234, -1234, 0u);
    drive_motor_app_step(&app, true, 1u);

    MT_CHECK(motor.coast[DRIVE_MOTOR_CHANNEL_A]);
    MT_CHECK(motor.coast[DRIVE_MOTOR_CHANNEL_B]);
    MT_CHECK(app.duty_cpercent[DRIVE_MOTOR_CHANNEL_A] == 0);
    MT_CHECK(app.duty_cpercent[DRIVE_MOTOR_CHANNEL_B] == 0);
}

static void test_duty_mode_passes_target_straight_through(void)
{
    fake_motor_t motor;
    fm_init(&motor);
    drive_motor_hw_t hw = make_fake_hw(&motor);
    rover_params_drive_t params = make_test_params();
    drive_motor_app_t app;
    drive_motor_app_init(&app, &hw, &params);

    drive_motor_app_on_cmd(&app, (uint8_t)DRIVE_MODE_DUTY, 5000, -3000, 0u);
    drive_motor_app_step(&app, true, 1u);

    MT_CHECK(app.duty_cpercent[DRIVE_MOTOR_CHANNEL_A] == 5000);
    MT_CHECK(app.duty_cpercent[DRIVE_MOTOR_CHANNEL_B] == -3000);
    MT_CHECK(motor.duty[DRIVE_MOTOR_CHANNEL_A] == 5000);
}

static void test_duty_mode_clamped_to_100_percent(void)
{
    fake_motor_t motor;
    fm_init(&motor);
    drive_motor_hw_t hw = make_fake_hw(&motor);
    rover_params_drive_t params = make_test_params();
    drive_motor_app_t app;
    drive_motor_app_init(&app, &hw, &params);

    drive_motor_app_on_cmd(&app, (uint8_t)DRIVE_MODE_DUTY, 32000, -32000, 0u);
    drive_motor_app_step(&app, true, 1u);

    MT_CHECK(app.duty_cpercent[DRIVE_MOTOR_CHANNEL_A] == 10000);
    MT_CHECK(app.duty_cpercent[DRIVE_MOTOR_CHANNEL_B] == -10000);
}

static void test_not_armed_forces_safe_output_regardless_of_mode(void)
{
    fake_motor_t motor;
    fm_init(&motor);
    drive_motor_hw_t hw = make_fake_hw(&motor);
    rover_params_drive_t params = make_test_params();
    params.brake_or_coast = 1u; /* brake while disarmed */
    drive_motor_app_t app;
    drive_motor_app_init(&app, &hw, &params);

    drive_motor_app_on_cmd(&app, (uint8_t)DRIVE_MODE_DUTY, 9000, 9000, 0u);
    drive_motor_app_step(&app, false, 1u);

    MT_CHECK(motor.brake[DRIVE_MOTOR_CHANNEL_A]);
    MT_CHECK(motor.brake[DRIVE_MOTOR_CHANNEL_B]);
    MT_CHECK(app.duty_cpercent[DRIVE_MOTOR_CHANNEL_A] == 0);
}

static void test_velocity_target_clamped_to_max_speed(void)
{
    fake_motor_t motor;
    fm_init(&motor);
    drive_motor_hw_t hw = make_fake_hw(&motor);
    rover_params_drive_t params = make_test_params();
    params.max_speed = 1000; /* 10 rad/s */
    drive_motor_app_t app;
    drive_motor_app_init(&app, &hw, &params);

    drive_motor_app_on_cmd(&app, (uint8_t)DRIVE_MODE_VELOCITY, 32000, -32000, 0u);

    MT_CHECK(app.target_raw[DRIVE_MOTOR_CHANNEL_A] == 1000);
    MT_CHECK(app.target_raw[DRIVE_MOTOR_CHANNEL_B] == -1000);
}

static void test_acceleration_ramp_limits_the_setpoint(void)
{
    fake_motor_t motor;
    fm_init(&motor);
    drive_motor_hw_t hw = make_fake_hw(&motor);
    rover_params_drive_t params = make_test_params();
    drive_motor_app_t app;
    drive_motor_app_init(&app, &hw, &params);

    /* accel_limit 5000 (50 rad/s^2) over a 1 ms tick ramps at most 5 (0.01 rad/s) per tick, so
     * reaching a 20000 target takes about 4000 ticks. */
    drive_motor_app_on_cmd(&app, (uint8_t)DRIVE_MODE_VELOCITY, 20000, 0, 5000u);
    drive_motor_app_step(&app, true, 1u);

    MT_CHECK(app.pid[DRIVE_MOTOR_CHANNEL_A].ramped_target_crad_s <= 5.5f);
    MT_CHECK(app.pid[DRIVE_MOTOR_CHANNEL_A].ramped_target_crad_s > 0.0f);

    /* Running it long enough reaches the full target. */
    for (int i = 0; i < 4500; i++) {
        drive_motor_app_step(&app, true, 1u);
    }
    MT_CHECK(app.pid[DRIVE_MOTOR_CHANNEL_A].ramped_target_crad_s >= 19999.0f);
}

static void test_zero_accel_limit_means_unlimited(void)
{
    fake_motor_t motor;
    fm_init(&motor);
    drive_motor_hw_t hw = make_fake_hw(&motor);
    rover_params_drive_t params = make_test_params();
    drive_motor_app_t app;
    drive_motor_app_init(&app, &hw, &params);

    drive_motor_app_on_cmd(&app, (uint8_t)DRIVE_MODE_VELOCITY, 5000, 0, 0u);
    drive_motor_app_step(&app, true, 1u);

    MT_CHECK(app.pid[DRIVE_MOTOR_CHANNEL_A].ramped_target_crad_s == 5000.0f);
}

static void test_velocity_pid_converges_toward_target(void)
{
    fake_motor_t motor;
    fm_init(&motor);
    drive_motor_hw_t hw = make_fake_hw(&motor);
    rover_params_drive_t params = make_test_params();
    drive_motor_app_t app;
    drive_motor_app_init(&app, &hw, &params);

    drive_motor_app_on_cmd(&app, (uint8_t)DRIVE_MODE_VELOCITY, 2000, 0, 0u);

    int16_t first_error = 0;
    for (int i = 0; i < 2000; i++) {
        drive_motor_app_step(&app, true, 1u);
        if (i == 0) {
            first_error = (int16_t)(2000 - app.measured_crad_s[DRIVE_MOTOR_CHANNEL_A]);
        }
    }
    int16_t last_error = (int16_t)(2000 - app.measured_crad_s[DRIVE_MOTOR_CHANNEL_A]);
    int32_t abs_first = first_error < 0 ? -first_error : first_error;
    int32_t abs_last = last_error < 0 ? -last_error : last_error;

    MT_CHECK(abs_last < abs_first);
    MT_CHECK(abs_last < 300); /* within 3 rad/s of a 20 rad/s target after 2 s */
}

static void test_overcurrent_cuts_output(void)
{
    fake_motor_t motor;
    fm_init(&motor);
    drive_motor_hw_t hw = make_fake_hw(&motor);
    rover_params_drive_t params = make_test_params();
    params.current_limit_ma = 100u; /* trivially low: any nonzero duty trips it */
    drive_motor_app_t app;
    drive_motor_app_init(&app, &hw, &params);

    drive_motor_app_on_cmd(&app, (uint8_t)DRIVE_MODE_DUTY, 5000, 0, 0u);
    drive_motor_app_step(&app, true, 1u);
    /* First tick: current reflects the *previous* (zero) duty, so it is not yet tripped. */
    MT_CHECK(app.duty_cpercent[DRIVE_MOTOR_CHANNEL_A] == 5000);

    drive_motor_app_step(&app, true, 1u);
    MT_CHECK(app.overcurrent[DRIVE_MOTOR_CHANNEL_A]);
    MT_CHECK(app.duty_cpercent[DRIVE_MOTOR_CHANNEL_A] == 0);
}

int main(void)
{
    MT_RUN(test_coast_mode_outputs_nothing);
    MT_RUN(test_duty_mode_passes_target_straight_through);
    MT_RUN(test_duty_mode_clamped_to_100_percent);
    MT_RUN(test_not_armed_forces_safe_output_regardless_of_mode);
    MT_RUN(test_velocity_target_clamped_to_max_speed);
    MT_RUN(test_acceleration_ramp_limits_the_setpoint);
    MT_RUN(test_zero_accel_limit_means_unlimited);
    MT_RUN(test_velocity_pid_converges_toward_target);
    MT_RUN(test_overcurrent_cuts_output);
    return mt_summary();
}
