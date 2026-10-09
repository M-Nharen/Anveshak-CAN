/* Top-level glue: ties the CAN driver, software tx queue, state machine, master tracking,
 * watchdogs, SYNC/command buffering, parameters and SERVICE together into the one object a
 * node application drives. Handles the class-0 network frames (ESTOP, ESTOP_RELEASE, SYNC,
 * NMT, HOST_HB) directly, plus this node's own DRIVE_CMD and SERVICE requests: those two use
 * the drive1 generated message layouts, which are byte-identical for every drive node (only
 * the CAN ID differs, by ids.h's per-node offset) -- see the design note in anv_node.c. */
#ifndef ANV_NODE_H
#define ANV_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acc_can.h"
#include "anv_board.h"
#include "anv_cmdwatch.h"
#include "anv_debug.h"
#include "anv_diag.h"
#include "anv_estop.h"
#include "anv_heartbeat.h"
#include "anv_master.h"
#include "anv_nmt.h"
#include "anv_params.h"
#include "anv_service.h"
#include "anv_state.h"
#include "anv_syncbuf.h"
#include "anv_txqueue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t node_id;
    uint8_t node_type;
    uint8_t protocol_version;

    const acc_can_driver_t *driver;
    anv_txqueue_t txq;

    anv_state_machine_t state;
    anv_master_tracker_t master;
    anv_cmd_watchdog_t cmd_wd;
    anv_sync_buf_t cmd_buf;
    anv_estop_latch_t estop;

    anv_param_store_t *params;
    anv_node_info_t info;

    anv_rate_sched_t hb_sched;
    uint8_t last_sync_cycle;

    const anv_board_hooks_t *board;

    uint32_t boot_ms;
    bool fault_active;
    acc_can_error_state_t last_error_state;

    /* Diagnostics: off until the application calls anv_node_diag_start(), so a node that has not
     * opted in sends no NODE_EVENT frames and reports debug level 0. */
    bool diag_on;
    anv_diag_t diag;
    anv_debug_t debug;
    anv_crash_record_t crash;
    bool crash_valid;
    uint8_t reset_cause;
    uint32_t self_test_failed;
    uint8_t prev_can_state;
    uint32_t prev_ack_errors;
} anv_node_t;

/* `txq_storage` must have room for ACC_CAN_PRIO_COUNT * txq_capacity_per_prio frames.
 * `params` must be a store the caller already initialised (anv_param_store_init +
 * anv_param_store_load); a node with no parameters still needs a store with count == 0. */
void anv_node_init(anv_node_t *node, uint8_t node_id, uint8_t node_type, uint8_t protocol_version,
                    const acc_can_driver_t *driver, acc_can_frame_t *txq_storage, size_t txq_capacity_per_prio,
                    anv_param_store_t *params, const anv_node_info_t *info, const anv_board_hooks_t *board,
                    uint32_t now_ms);

/* Boot-time diagnostics; call after anv_node_init() and loading parameters, before
 * anv_node_boot_complete(). Loads the flash log and bumps the boot count, turns the last
 * reset cause and any saved crash record into events, and runs the board self-test (a failure
 * raises SELF_TEST_FAIL and puts the node in FAULT). `debug_default` is the debug_default
 * parameter. Returns the self-test failure bitmask (0 = passed). */
uint32_t anv_node_diag_start(anv_node_t *node, uint8_t debug_default, uint32_t now_ms);

/* Reports an application-detected condition (OVERCURRENT_A, ...) or clears it. No-ops until
 * anv_node_diag_start() has run. See anv_diag.h. */
void anv_node_raise(anv_node_t *node, uint16_t code, uint16_t detail, uint32_t now_ms);
void anv_node_lower(anv_node_t *node, uint16_t code, uint16_t detail, uint32_t now_ms);
void anv_node_event(anv_node_t *node, uint16_t code, uint16_t detail, uint32_t now_ms);

/* Call once self-test has passed: BOOT -> DISARMED. */
void anv_node_boot_complete(anv_node_t *node);

void anv_node_on_rx_frame(anv_node_t *node, const acc_can_frame_t *frame, uint32_t now_ms);

/* Call once per main-loop iteration: feeds the hardware watchdog, checks the command
 * watchdog, checks for bus-off and starts recovery, and sends NODE_HB when due. */
void anv_node_tick(anv_node_t *node, uint32_t now_ms);

anv_state_t anv_node_state(const anv_node_t *node);
bool anv_node_no_master(const anv_node_t *node, uint32_t now_ms);

void anv_node_report_fault(anv_node_t *node);
void anv_node_clear_fault_condition(anv_node_t *node);

/* rate_hz == 0 disables NODE_HB. Call again whenever the rate_hb_hz parameter changes. */
void anv_node_set_hb_rate(anv_node_t *node, uint32_t rate_hz, uint32_t now_ms);

bool anv_node_send(anv_node_t *node, acc_can_priority_t prio, const acc_can_frame_t *frame);
size_t anv_node_drain_tx(anv_node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* ANV_NODE_H */
