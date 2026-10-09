/* Per-channel velocity PID, acceleration ramp and mode handling for one drive node's two motor
 * channels (docs/architecture.md, "Node firmware" -- "The 20 ms control cycle", "Command
 * checks on every node"). Pure logic: hardware access goes through drive_motor_hw_t, so this
 * module builds and runs on the host (firmware/nodes/drive_stm32/tests/test_drive_motor.c) the
 * same way it does cross-compiled, with no HAL dependency at all.
 */
#ifndef DRIVE_MOTOR_H
#define DRIVE_MOTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "params_drive.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRIVE_MOTOR_CHANNEL_A 0u
#define DRIVE_MOTOR_CHANNEL_B 1u
#define DRIVE_MOTOR_CHANNEL_COUNT 2u

/* Matches DRIVE_CMD's MODE field (docs/architecture.md, Message catalog). */
typedef enum {
    DRIVE_MODE_COAST = 0,
    DRIVE_MODE_BRAKE = 1,
    DRIVE_MODE_VELOCITY = 2,
    DRIVE_MODE_DUTY = 3,
} drive_mode_t;

/* Hardware access, implemented against real timers/GPIO/ADC on the board (src/board_hw_*.c)
 * and against a simple motor model in tests. `duty_cpercent` is +-10000 = +-100%, sign gives
 * direction. `read_encoder` returns a free-running count (wraps like a real timer counter). */
typedef struct {
    void *ctx;
    void (*set_duty)(void *ctx, uint8_t channel, int16_t duty_cpercent);
    void (*set_coast)(void *ctx, uint8_t channel);
    void (*set_brake)(void *ctx, uint8_t channel);
    int32_t (*read_encoder)(void *ctx, uint8_t channel);
    uint32_t (*read_current_ma)(void *ctx, uint8_t channel);
} drive_motor_hw_t;

typedef struct {
    float integral;
    float prev_measured;
    float ramped_target_crad_s;
} drive_pid_channel_t;

typedef struct {
    const drive_motor_hw_t *hw;
    const rover_params_drive_t *params; /* caller-owned; read fresh each step() */

    drive_mode_t mode;
    /* Raw DRIVE_CMD targets: 0.01 rad/s in VELOCITY mode, 0.01% duty in DUTY mode -- same
     * encoding DRIVE_CMD uses, so no conversion happens until step() knows the mode. */
    int16_t target_raw[DRIVE_MOTOR_CHANNEL_COUNT];
    uint16_t accel_limit_crad_s2;

    drive_pid_channel_t pid[DRIVE_MOTOR_CHANNEL_COUNT];
    int32_t last_encoder[DRIVE_MOTOR_CHANNEL_COUNT];
    int16_t measured_crad_s[DRIVE_MOTOR_CHANNEL_COUNT];
    int16_t duty_cpercent[DRIVE_MOTOR_CHANNEL_COUNT];
    uint32_t current_ma[DRIVE_MOTOR_CHANNEL_COUNT];
    bool overcurrent[DRIVE_MOTOR_CHANNEL_COUNT];
} drive_motor_app_t;

void drive_motor_app_init(drive_motor_app_t *app, const drive_motor_hw_t *hw, const rover_params_drive_t *params);

/* Call once a new DRIVE_CMD is applied at SYNC (docs/architecture.md: "Nodes store a new
 * command when it arrives but only apply it at the next SYNC"). Targets are clamped to
 * +-max_speed here; mode and accel_limit are stored as received. */
void drive_motor_app_on_cmd(drive_motor_app_t *app, uint8_t mode, int16_t target_a, int16_t target_b,
                            uint16_t accel_limit);

/* Call at DRIVE_PID_RATE_HZ (board_config.h, 1 kHz). Reads encoders/current, runs the ramp and
 * PID (VELOCITY) or passes the target straight through (DUTY), and always drives safe outputs
 * (coast or brake, per params->brake_or_coast) when `armed` is false, regardless of mode. */
void drive_motor_app_step(drive_motor_app_t *app, bool armed, uint32_t dt_ms);

#ifdef __cplusplus
}
#endif

#endif /* DRIVE_MOTOR_H */
