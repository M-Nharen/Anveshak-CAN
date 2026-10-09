/* Wires anveshak_can_core's anv_node_t together with drive_motor.h's per-channel PID into the
 * one object a drive node's board bring-up code drives (src/main_f446.c, src/main_g431.c), and
 * adds the two things DRIVE_CMD/SERVICE/NODE_HB generic handling in anv_node.c does not cover:
 * DRIVE_FB_A/_B sent right after every SYNC, and DRIVE_TELEM_0/_1 at the telemetry rate
 * parameter (docs/architecture.md, "Node firmware" and "Message catalog").
 *
 * Like anv_node.c's own design note, DRIVE_FB_A/_B and DRIVE_TELEM_0/_1 are generated once per
 * drive node (drive1..drive6) with byte-identical layouts; this module uses drive1's generated
 * structs as the canonical shape for any drive node and computes the real CAN ID at runtime
 * from this node's own ID via ids.h.
 */
#ifndef DRIVE_APP_H
#define DRIVE_APP_H

#include <stddef.h>
#include <stdint.h>

#include "anv_node.h"
#include "drive_motor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRIVE_NODE_TYPE 1u /* NODE_HB's NODE_TYPE: 1 drive (docs/architecture.md) */
#define DRIVE_PROTOCOL_VERSION 1u

/* Board telemetry inputs DRIVE_TELEM_0 needs that drive_motor_hw_t has no reason to know
 * about (it is motor-channel-shaped, these are board-wide). */
typedef struct {
    void *ctx;
    uint16_t (*read_supply_mv)(void *ctx);
    int8_t (*read_temp_driver_a)(void *ctx);
    int8_t (*read_temp_driver_b)(void *ctx);
    int8_t (*read_temp_mcu)(void *ctx);
} drive_telem_hw_t;

typedef struct {
    anv_node_t node;
    drive_motor_app_t motor;
    anv_param_store_t *param_store;
    rover_params_drive_t params_snapshot;
    const drive_telem_hw_t *telem_hw;

    anv_rate_sched_t telem_sched;
    uint8_t last_telem_rate_hz; /* effective rate: parameter, or 1 Hz at debug level 0 */
    uint8_t last_hb_rate_hz;

    anv_rate_sched_t debug_sched; /* DRIVE_DEBUG_A/B: 10 Hz at debug level 2, 50 Hz at 3, off below */
    uint32_t last_debug_rate_hz;
    uint32_t sync_count;          /* SYNCs since the last DRIVE_FB (rate_fb_hz divides the 50 Hz cycle) */
    uint32_t last_now_ms;         /* most recent time seen, for events raised from motor_step() */
} drive_app_t;

/* Converts the generated ROVER_PARAMS_DRIVE_TABLE into the core's generic descriptor shape
 * (anv_params.h) -- a plain metadata copy, not byte packing. `out` must have room for
 * ROVER_PARAMS_DRIVE_TABLE_LEN entries; pass it to anv_param_store_init() before
 * drive_app_init(). */
void drive_app_build_param_descriptors(anv_param_descriptor_t *out);

void drive_app_init(drive_app_t *app, uint8_t node_id, const acc_can_driver_t *driver, acc_can_frame_t *txq_storage,
                     size_t txq_capacity_per_prio, anv_param_store_t *params, const anv_node_info_t *info,
                     const anv_board_hooks_t *board, const drive_motor_hw_t *motor_hw,
                     const drive_telem_hw_t *telem_hw, uint32_t now_ms);

/* Wraps anv_node_on_rx_frame(): on SYNC, also applies the newly-active command to the motor
 * app and sends DRIVE_FB_A/_B immediately. */
void drive_app_on_rx_frame(drive_app_t *app, const acc_can_frame_t *frame, uint32_t now_ms);

/* Call at the board's PID rate (board_config.h DRIVE_PID_RATE_HZ, 1 kHz): refreshes the live
 * parameter snapshot and runs the motor ramp/PID for both channels. `dt_ms` is the time since
 * the previous call (board_config.h's caller knows the rate; this module stays board-agnostic
 * so it keeps building and testing on the host). */
void drive_app_motor_step(drive_app_t *app, uint32_t dt_ms);

/* Call once per main-loop iteration: runs anv_node_tick() (watchdogs, bus-off, NODE_HB, events)
 * and sends DRIVE_TELEM_0/_1 and, at debug level 2+, DRIVE_DEBUG_A/_B when due. */
void drive_app_tick(drive_app_t *app, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* DRIVE_APP_H */
