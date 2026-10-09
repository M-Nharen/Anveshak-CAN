/* Design note on using generated code generically: ESTOP, ESTOP_RELEASE, SYNC, NMT and
 * HOST_HB are single, network-wide messages in the DBC, so their generated structs
 * (rover_estop_t etc.) are already node-type-agnostic and used directly below.
 *
 * DRIVE_CMD and SERVICE are generated once per drive node (drive1..drive6, defs/rover.dbc)
 * because each has its own CAN ID, but cantools emits byte-identical struct layouts for all
 * six (same fields, same order -- only the message name differs). This module therefore
 * treats drive1's generated structs as the canonical shape for *any* drive node's command
 * and service traffic, and computes the actual CAN ID at runtime from this node's own ID via
 * ids.h, instead of the per-node ROVER_DRIVEn_*_FRAME_ID macros. That keeps one shared core
 * library working for all six drive nodes without hand-written byte packing anywhere. */
#include "anv_node.h"

#include <string.h>

#include "ids.h"
#include "rover.h"

static void enforce_safe_outputs(anv_node_t *node)
{
    if (anv_state_get(&node->state) != ANV_STATE_ARMED && node->board->set_outputs_off != NULL) {
        node->board->set_outputs_off(node->board->ctx);
    }
}

void anv_node_init(anv_node_t *node, uint8_t node_id, uint8_t node_type, uint8_t protocol_version,
                    const acc_can_driver_t *driver, acc_can_frame_t *txq_storage, size_t txq_capacity_per_prio,
                    anv_param_store_t *params, const anv_node_info_t *info, const anv_board_hooks_t *board,
                    uint32_t now_ms)
{
    memset(node, 0, sizeof(*node));

    node->node_id = node_id;
    node->node_type = node_type;
    node->protocol_version = protocol_version;
    node->driver = driver;
    node->params = params;
    node->board = board;
    node->boot_ms = now_ms;

    if (info != NULL) {
        node->info = *info;
    }

    anv_txqueue_init(&node->txq, txq_storage, txq_capacity_per_prio);
    anv_state_init(&node->state);
    anv_master_init(&node->master);
    anv_cmd_watchdog_init(&node->cmd_wd);
    anv_sync_buf_init(&node->cmd_buf);
    anv_estop_init(&node->estop);
    anv_rate_sched_init(&node->hb_sched, 0, now_ms);

    enforce_safe_outputs(node);
}

void anv_node_boot_complete(anv_node_t *node)
{
    (void)anv_state_handle(&node->state, ANV_EVENT_BOOT_COMPLETE);
    enforce_safe_outputs(node);
}

anv_state_t anv_node_state(const anv_node_t *node)
{
    return anv_state_get(&node->state);
}

bool anv_node_no_master(const anv_node_t *node, uint32_t now_ms)
{
    return anv_master_lost(&node->master, now_ms, 300u);
}

void anv_node_report_fault(anv_node_t *node)
{
    node->fault_active = true;
    (void)anv_state_handle(&node->state, ANV_EVENT_FAULT);
    enforce_safe_outputs(node);
}

void anv_node_clear_fault_condition(anv_node_t *node)
{
    node->fault_active = false;
}

void anv_node_set_hb_rate(anv_node_t *node, uint32_t rate_hz, uint32_t now_ms)
{
    anv_rate_sched_set_rate(&node->hb_sched, rate_hz, now_ms);
}

bool anv_node_send(anv_node_t *node, acc_can_priority_t prio, const acc_can_frame_t *frame)
{
    return anv_txqueue_push(&node->txq, prio, frame);
}

size_t anv_node_drain_tx(anv_node_t *node)
{
    return anv_txqueue_drain(&node->txq, node->driver);
}

static void handle_estop(anv_node_t *node)
{
    anv_estop_trigger(&node->estop);
    (void)anv_state_handle(&node->state, ANV_EVENT_ESTOP);
    enforce_safe_outputs(node);
}

static void handle_estop_release(anv_node_t *node, const acc_can_frame_t *frame)
{
    struct rover_estop_release_t msg;
    if (rover_estop_release_unpack(&msg, frame->data, frame->dlc) != 0) {
        return;
    }
    if (!anv_master_check(&node->master, msg.master_node_id, msg.master_term, 0xFFu)) {
        return;
    }
    anv_estop_release(&node->estop);
    (void)anv_state_handle(&node->state, ANV_EVENT_ESTOP_RELEASED);
    enforce_safe_outputs(node);
}

static void handle_sync(anv_node_t *node, const acc_can_frame_t *frame)
{
    struct rover_sync_t msg;
    if (rover_sync_unpack(&msg, frame->data, frame->dlc) == 0) {
        node->last_sync_cycle = msg.cycle_counter;
    }
    anv_sync_buf_apply(&node->cmd_buf);
}

static void handle_nmt(anv_node_t *node, const acc_can_frame_t *frame)
{
    struct rover_nmt_t msg;
    if (rover_nmt_unpack(&msg, frame->data, frame->dlc) != 0) {
        return;
    }
    if (!anv_nmt_targets_me(msg.target_node, node->node_id)) {
        return;
    }
    if (!anv_master_check(&node->master, msg.master_node_id, msg.master_term, 0xFFu)) {
        return;
    }

    switch ((anv_nmt_command_t)msg.command) {
    case ANV_NMT_CMD_ARM:
        (void)anv_state_handle(&node->state, ANV_EVENT_ARM);
        break;
    case ANV_NMT_CMD_DISARM:
        (void)anv_state_handle(&node->state, ANV_EVENT_DISARM);
        break;
    case ANV_NMT_CMD_RESET:
        if (node->board->request_reset != NULL) {
            node->board->request_reset(node->board->ctx);
        }
        break;
    case ANV_NMT_CMD_ENTER_BOOTLOADER:
        /* Accepted only from DISARMED (the state machine refuses it otherwise). Outputs go
         * off first, then the "stay in bootloader" flag is set and the MCU resets into the
         * bootloader, which sees the flag and waits for the host. */
        if (anv_state_handle(&node->state, ANV_EVENT_ENTER_BOOTLOADER)) {
            enforce_safe_outputs(node);
            if (node->board->enter_bootloader != NULL) {
                node->board->enter_bootloader(node->board->ctx);
            }
            if (node->board->request_reset != NULL) {
                node->board->request_reset(node->board->ctx);
            }
        }
        break;
    case ANV_NMT_CMD_FAULT_RESET:
        if (!node->fault_active) {
            (void)anv_state_handle(&node->state, ANV_EVENT_FAULT_RESET);
        }
        break;
    default:
        break;
    }
    enforce_safe_outputs(node);
}

static void handle_host_hb(anv_node_t *node, uint8_t sender_node_id, const acc_can_frame_t *frame,
                            uint32_t now_ms)
{
    struct rover_host_hb_orin_t msg;
    if (rover_host_hb_orin_unpack(&msg, frame->data, frame->dlc) != 0) {
        return;
    }
    bool master_changed =
        anv_master_on_host_hb(&node->master, sender_node_id, (anv_host_role_t)msg.role, msg.term, now_ms);
    if (master_changed) {
        (void)anv_state_handle(&node->state, ANV_EVENT_DISARM);
        anv_cmd_watchdog_reset(&node->cmd_wd);
        enforce_safe_outputs(node);
    }
}

static void handle_drive_cmd(anv_node_t *node, const acc_can_frame_t *frame, uint32_t now_ms)
{
    struct rover_drive1_cmd_t msg;
    if (rover_drive1_cmd_unpack(&msg, frame->data, frame->dlc) != 0) {
        return;
    }
    if (!anv_master_check(&node->master, msg.sender_host, msg.term_low, 0x03u)) {
        return;
    }
    anv_cmd_watchdog_on_command(&node->cmd_wd, msg.seq, now_ms);
    anv_sync_buf_store(&node->cmd_buf, frame->data, frame->dlc);
}

static void handle_service_request(anv_node_t *node, uint8_t requester_sub, const acc_can_frame_t *frame)
{
    struct rover_drive1_service_req_orin_t req_msg;
    if (rover_drive1_service_req_orin_unpack(&req_msg, frame->data, frame->dlc) != 0) {
        return;
    }

    anv_service_request_t req = {
        .opcode = req_msg.opcode,
        .transaction_id = req_msg.transaction_id,
        .param_index = req_msg.param_index,
        .value_raw = req_msg.value_raw,
    };
    anv_service_response_t resp;
    bool armed = anv_state_get(&node->state) == ANV_STATE_ARMED;
    if (!anv_service_handle_request(node->params, armed, &node->info, &req, &resp)) {
        return;
    }

    struct rover_drive1_service_resp_orin_t resp_msg = {
        .opcode = resp.opcode,
        .status = resp.status,
        .transaction_id = resp.transaction_id,
        .param_index = resp.param_index,
        .value_raw = resp.value_raw,
    };
    acc_can_frame_t resp_frame;
    resp_frame.id = ROVER_CAN_ID(ROVER_CLASS_SERVICE, node->node_id, (uint8_t)(requester_sub + 4u));
    int packed = rover_drive1_service_resp_orin_pack(resp_frame.data, &resp_msg, sizeof(resp_frame.data));
    if (packed < 0) {
        return;
    }
    resp_frame.dlc = (uint8_t)packed;
    (void)anv_node_send(node, ACC_CAN_PRIO_SERVICE, &resp_frame);
}

static void send_node_hb(anv_node_t *node, uint32_t now_ms)
{
    struct rover_drive1_node_hb_t msg = {
        .state = (uint8_t)anv_state_get(&node->state),
        .node_type = node->node_type,
        .protocol_version = node->protocol_version,
        .can_error_state = (uint8_t)node->last_error_state.state,
        .bootloader_running = (anv_state_get(&node->state) == ANV_STATE_BOOTLOADER) ? 1u : 0u,
        .unsaved_param_changes = anv_param_store_dirty(node->params) ? 1u : 0u,
        .debug_level = 0u,
        .fw_build_id = (uint16_t)node->info.git_build_id,
        .uptime_s = (uint16_t)((now_ms - node->boot_ms) / 1000u),
    };

    acc_can_frame_t frame;
    frame.id = ROVER_CAN_ID(ROVER_CLASS_HEARTBEAT, node->node_id, 0u);
    int packed = rover_drive1_node_hb_pack(frame.data, &msg, sizeof(frame.data));
    if (packed < 0) {
        return;
    }
    frame.dlc = (uint8_t)packed;
    (void)anv_node_send(node, ACC_CAN_PRIO_TELEMETRY, &frame);
}

void anv_node_on_rx_frame(anv_node_t *node, const acc_can_frame_t *frame, uint32_t now_ms)
{
    uint8_t class = ROVER_ID_CLASS(frame->id);
    uint8_t id_node = ROVER_ID_NODE(frame->id);
    uint8_t sub = ROVER_ID_SUB(frame->id);

    if (frame->id == ROVER_ESTOP_FRAME_ID) {
        handle_estop(node);
        return;
    }
    if (frame->id == ROVER_ESTOP_RELEASE_FRAME_ID) {
        handle_estop_release(node, frame);
        return;
    }
    if (frame->id == ROVER_SYNC_FRAME_ID) {
        handle_sync(node, frame);
        return;
    }
    if (frame->id == ROVER_NMT_FRAME_ID) {
        handle_nmt(node, frame);
        return;
    }
    if (frame->id == ROVER_HOST_HB_ORIN_FRAME_ID) {
        handle_host_hb(node, ROVER_NODE_ORIN, frame, now_ms);
        return;
    }
    if (frame->id == ROVER_HOST_HB_RPI_FRAME_ID) {
        handle_host_hb(node, ROVER_NODE_RPI, frame, now_ms);
        return;
    }
    if (frame->id == ROVER_HOST_HB_TOOL_FRAME_ID) {
        handle_host_hb(node, ROVER_NODE_TOOL, frame, now_ms);
        return;
    }

    if (id_node != node->node_id) {
        return;
    }
    if (class == ROVER_CLASS_COMMAND && sub == 0u) {
        handle_drive_cmd(node, frame, now_ms);
        return;
    }
    if (class == ROVER_CLASS_SERVICE && sub <= 2u) {
        handle_service_request(node, sub, frame);
        return;
    }
}

void anv_node_tick(anv_node_t *node, uint32_t now_ms)
{
    if (node->board->feed_hw_watchdog != NULL) {
        node->board->feed_hw_watchdog(node->board->ctx);
    }

    if (anv_state_get(&node->state) == ANV_STATE_ARMED && !anv_cmd_watchdog_is_fresh(&node->cmd_wd, now_ms)) {
        (void)anv_state_handle(&node->state, ANV_EVENT_CMD_TIMEOUT);
    }
    enforce_safe_outputs(node);

    if (node->driver->get_error_state != NULL &&
        node->driver->get_error_state(node->driver->ctx, &node->last_error_state) == ACC_CAN_OK) {
        if (node->last_error_state.state == ACC_CAN_BUS_OFF) {
            /* Bus-off means commands from the master can no longer be trusted to arrive at
             * all: force outputs off directly, regardless of the reported ARMED state. */
            if (node->board->set_outputs_off != NULL) {
                node->board->set_outputs_off(node->board->ctx);
            }
            if (node->driver->start_recovery != NULL) {
                (void)node->driver->start_recovery(node->driver->ctx);
            }
        }
    }

    if (anv_rate_sched_due(&node->hb_sched, now_ms)) {
        send_node_hb(node, now_ms);
    }
}
