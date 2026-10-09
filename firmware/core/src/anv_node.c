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

#include "errors.h"
#include "ids.h"
#include "rover.h"

static void enforce_safe_outputs(anv_node_t *node)
{
    if (anv_state_get(&node->state) != ANV_STATE_ARMED && node->board->set_outputs_off != NULL) {
        node->board->set_outputs_off(node->board->ctx);
    }
}

/* --- diagnostics glue (anv_diag.h / anv_debug.h) --- */

static bool node_event_sink(void *ctx, const anv_diag_msg_t *msg)
{
    anv_node_t *node = (anv_node_t *)ctx;
    struct rover_drive1_node_event_t ev = {
        .severity = msg->severity,
        .cleared = msg->cleared ? 1u : 0u,
        .error_code = msg->code,
        .event_seq = msg->seq,
        .detail = msg->detail,
        .uptime_0_1_s = msg->uptime_ds,
    };
    acc_can_frame_t frame;
    frame.id = ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, node->node_id, 7u);
    int packed = rover_drive1_node_event_pack(frame.data, &ev, sizeof(frame.data));
    if (packed < 0) {
        return true; /* can't happen with a fixed layout; don't retry forever */
    }
    frame.dlc = (uint8_t)packed;
    return anv_node_send(node, ACC_CAN_PRIO_TELEMETRY, &frame);
}

static bool node_event_gate(void *ctx, uint8_t severity)
{
    return anv_debug_event_allowed(&((anv_node_t *)ctx)->debug, severity);
}

void anv_node_raise(anv_node_t *node, uint16_t code, uint16_t detail, uint32_t now_ms)
{
    if (node->diag_on) {
        anv_diag_report(&node->diag, code, detail, now_ms);
    }
}

void anv_node_lower(anv_node_t *node, uint16_t code, uint16_t detail, uint32_t now_ms)
{
    if (node->diag_on) {
        anv_diag_clear(&node->diag, code, detail, now_ms);
    }
}

void anv_node_event(anv_node_t *node, uint16_t code, uint16_t detail, uint32_t now_ms)
{
    if (node->diag_on) {
        anv_diag_event(&node->diag, code, detail, now_ms);
    }
}

uint32_t anv_node_diag_start(anv_node_t *node, uint8_t debug_default, uint32_t now_ms)
{
    const anv_board_hooks_t *board = node->board;

    anv_debug_init(&node->debug, debug_default);
    anv_diag_init(&node->diag, board, now_ms);
    anv_diag_set_sink(&node->diag, node_event_sink, node_event_gate, node);
    node->diag_on = true;
    node->prev_can_state = (uint8_t)ACC_CAN_ERROR_ACTIVE;

    if (board->take_crash_record != NULL) {
        node->crash_valid = board->take_crash_record(board->ctx, &node->crash);
    }
    node->reset_cause = (board->reset_cause != NULL) ? board->reset_cause(board->ctx) : (uint8_t)ANV_RESET_UNKNOWN;

    /* What the last reset was, as events. The detail is the low half of the crash PC when the
     * crash handler left one (the full record is in the INFO pages). */
    uint16_t pc_low = node->crash_valid ? (uint16_t)(node->crash.pc & 0xFFFFu) : 0u;
    switch ((anv_reset_cause_t)node->reset_cause) {
    case ANV_RESET_WATCHDOG:
        anv_diag_event(&node->diag, ROVER_ERR_WATCHDOG_RESET, 0u, now_ms);
        break;
    case ANV_RESET_BROWNOUT:
        anv_diag_event(&node->diag, ROVER_ERR_BROWNOUT_RESET, 0u, now_ms);
        break;
    case ANV_RESET_HARD_FAULT:
    case ANV_RESET_PANIC:
        anv_diag_event(&node->diag, ROVER_ERR_HARD_FAULT, pc_low, now_ms);
        break;
    case ANV_RESET_STACK_OVERFLOW:
        anv_diag_event(&node->diag, ROVER_ERR_STACK_OVERFLOW, pc_low, now_ms);
        break;
    default:
        break;
    }

    node->self_test_failed = (board->self_test != NULL) ? board->self_test(board->ctx) : 0u;
    if (node->self_test_failed != 0u) {
        anv_diag_report(&node->diag, ROVER_ERR_SELF_TEST_FAIL, (uint16_t)(node->self_test_failed & 0xFFFFu), now_ms);
        anv_node_report_fault(node);
    }
    return node->self_test_failed;
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
    if (node->diag_on) {
        rate_hz = anv_rate_clamp_hb(rate_hz); /* a slow heartbeat would make a healthy node look missing */
    }
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

static void bad_frame(anv_node_t *node, const acc_can_frame_t *frame, uint32_t now_ms)
{
    anv_node_event(node, ROVER_ERR_BAD_FRAME, (uint16_t)(frame->id & 0x7FFu), now_ms);
}

static void handle_estop(anv_node_t *node, uint32_t now_ms)
{
    anv_node_raise(node, ROVER_ERR_ESTOP_CAN, 0u, now_ms);
    anv_estop_trigger(&node->estop);
    (void)anv_state_handle(&node->state, ANV_EVENT_ESTOP);
    enforce_safe_outputs(node);
}

static void handle_estop_release(anv_node_t *node, const acc_can_frame_t *frame, uint32_t now_ms)
{
    struct rover_estop_release_t msg;
    if (rover_estop_release_unpack(&msg, frame->data, frame->dlc) != 0) {
        return;
    }
    if (!anv_master_check(&node->master, msg.master_node_id, msg.master_term, 0xFFu)) {
        return;
    }
    anv_estop_release(&node->estop);
    anv_node_lower(node, ROVER_ERR_ESTOP_CAN, 0u, now_ms);
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
        if (node->board->enter_bootloader != NULL) {
            node->board->enter_bootloader(node->board->ctx);
        }
        (void)anv_state_handle(&node->state, ANV_EVENT_ENTER_BOOTLOADER);
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
        anv_node_event(node, ROVER_ERR_MASTER_CHANGED, sender_node_id, now_ms);
        (void)anv_state_handle(&node->state, ANV_EVENT_DISARM);
        anv_cmd_watchdog_reset(&node->cmd_wd);
        enforce_safe_outputs(node);
    }
}

static void handle_drive_cmd(anv_node_t *node, const acc_can_frame_t *frame, uint32_t now_ms)
{
    struct rover_drive1_cmd_t msg;
    if (rover_drive1_cmd_unpack(&msg, frame->data, frame->dlc) != 0) {
        bad_frame(node, frame, now_ms);
        return;
    }
    if (!anv_master_check(&node->master, msg.sender_host, msg.term_low, 0x03u)) {
        anv_node_event(node, ROVER_ERR_CMD_FROM_NON_MASTER, msg.sender_host, now_ms);
        return;
    }
    anv_node_lower(node, ROVER_ERR_CMD_TIMEOUT, 0u, now_ms);
    anv_cmd_watchdog_on_command(&node->cmd_wd, msg.seq, now_ms);
    anv_sync_buf_store(&node->cmd_buf, frame->data, frame->dlc);
}

static void send_service_response(anv_node_t *node, uint8_t requester_sub, const anv_service_response_t *resp)
{
    struct rover_drive1_service_resp_orin_t resp_msg = {
        .opcode = resp->opcode,
        .status = resp->status,
        .transaction_id = resp->transaction_id,
        .param_index = resp->param_index,
        .value_raw = resp->value_raw,
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

/* INFO pages 16 and up: crash record, boot/log counters and CAN error counts by type. These
 * have no room in TELEM_0/1, so they are read on demand (page numbers in the README). */
static bool diag_info_page(const anv_node_t *node, uint16_t index, uint32_t *value)
{
    if (index >= 32u && index < 32u + ACC_CAN_ERRTYPE_COUNT) {
        *value = node->last_error_state.error_type_counts[index - 32u];
        return true;
    }
    switch (index) {
    case 16: *value = node->crash_valid ? 1u : 0u; return true;
    case 17: *value = node->crash.pc; return true;
    case 18: *value = node->crash.lr; return true;
    case 19: *value = node->crash.xpsr; return true;
    case 20: *value = node->crash.cfsr; return true;
    case 21: *value = node->crash.hfsr; return true;
    case 22: *value = node->crash.mmfar; return true;
    case 23: *value = node->crash.bfar; return true;
    case 24: *value = node->diag.boot_count; return true;
    case 25: *value = anv_diag_log_count(&node->diag); return true;
    case 26: *value = node->reset_cause; return true;
    case 27: *value = node->self_test_failed; return true;
    case 28: *value = node->crash.extra; return true;
    default: return false;
    }
}

/* LOG_READ answers with two frames so a whole entry fits (8 bytes can't hold it):
 *   frame 1: index = n,          status 0,     value = code << 16 | detail
 *   frame 2: index = n | 0x8000, status = flags, value = uptime_0_1_s << 16 | boot_count
 * n counts from the newest entry (0). Severity follows from the code. */
static bool handle_log_service(anv_node_t *node, uint8_t requester_sub, const anv_service_request_t *req)
{
    anv_service_response_t resp = {
        .opcode = req->opcode,
        .status = 0u,
        .transaction_id = req->transaction_id,
        .param_index = req->param_index,
        .value_raw = 0u,
    };

    if (req->opcode == (uint8_t)ANV_SERVICE_OP_LOG_CLEAR) {
        if (anv_state_get(&node->state) == ANV_STATE_ARMED) {
            resp.status = (uint8_t)ANV_PARAM_STATUS_NOT_ALLOWED_WHILE_ARMED;
        } else {
            anv_diag_log_clear(&node->diag, node->board->now_ms != NULL ? node->board->now_ms(node->board->ctx) : 0u);
        }
        send_service_response(node, requester_sub, &resp);
        return true;
    }

    if (req->opcode == (uint8_t)ANV_SERVICE_OP_LOG_READ) {
        anv_log_entry_t e;
        if (!anv_diag_log_get(&node->diag, req->param_index, &e)) {
            resp.status = (uint8_t)ANV_PARAM_STATUS_BAD_INDEX;
            send_service_response(node, requester_sub, &resp);
            return true;
        }
        resp.value_raw = ((uint32_t)e.code << 16) | e.detail;
        send_service_response(node, requester_sub, &resp);
        resp.param_index = (uint16_t)(req->param_index | 0x8000u);
        resp.status = e.flags;
        resp.value_raw = ((uint32_t)e.uptime_ds << 16) | e.boot_count;
        send_service_response(node, requester_sub, &resp);
        return true;
    }

    if (req->opcode == (uint8_t)ANV_SERVICE_OP_INFO && req->param_index >= 16u) {
        uint32_t value;
        if (diag_info_page(node, req->param_index, &value)) {
            resp.value_raw = value;
        } else {
            resp.status = (uint8_t)ANV_PARAM_STATUS_BAD_INDEX;
        }
        send_service_response(node, requester_sub, &resp);
        return true;
    }
    return false;
}

static void handle_service_request(anv_node_t *node, uint8_t requester_sub, const acc_can_frame_t *frame,
                                    uint32_t now_ms)
{
    struct rover_drive1_service_req_orin_t req_msg;
    if (rover_drive1_service_req_orin_unpack(&req_msg, frame->data, frame->dlc) != 0) {
        bad_frame(node, frame, now_ms);
        return;
    }

    anv_service_request_t req = {
        .opcode = req_msg.opcode,
        .transaction_id = req_msg.transaction_id,
        .param_index = req_msg.param_index,
        .value_raw = req_msg.value_raw,
    };
    if (node->diag_on && handle_log_service(node, requester_sub, &req)) {
        return;
    }

    anv_service_response_t resp;
    bool armed = anv_state_get(&node->state) == ANV_STATE_ARMED;
    if (!anv_service_handle_request(node->params, armed, &node->info, &req, &resp)) {
        return;
    }
    send_service_response(node, requester_sub, &resp);
}

static void handle_debug_level(anv_node_t *node, const acc_can_frame_t *frame, uint32_t now_ms)
{
    struct rover_debug_level_t msg;
    if (!node->diag_on) {
        return;
    }
    if (rover_debug_level_unpack(&msg, frame->data, frame->dlc) != 0) {
        bad_frame(node, frame, now_ms);
        return;
    }
    if (!anv_nmt_targets_me(msg.target_node, node->node_id)) {
        return;
    }
    if (!anv_master_check(&node->master, msg.master_node_id, msg.master_term, 0xFFu)) {
        return;
    }
    (void)anv_debug_apply(&node->debug, msg.level, msg.category_mask, msg.auto_off_timer_min, now_ms);
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
        .debug_level = node->diag_on ? anv_debug_level(&node->debug) : 0u,
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
        handle_estop(node, now_ms);
        return;
    }
    if (frame->id == ROVER_ESTOP_RELEASE_FRAME_ID) {
        handle_estop_release(node, frame, now_ms);
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
    if (frame->id == ROVER_DEBUG_LEVEL_FRAME_ID) {
        handle_debug_level(node, frame, now_ms);
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
        handle_service_request(node, sub, frame, now_ms);
        return;
    }
}

/* Turns CAN controller state changes into events: passive, bus-off, recovered, ACK errors. */
static void track_can_errors(anv_node_t *node, uint32_t now_ms)
{
    const acc_can_error_state_t *err = &node->last_error_state;
    uint8_t prev = node->prev_can_state;
    node->prev_can_state = (uint8_t)err->state;

    if (err->state == ACC_CAN_ERROR_PASSIVE) {
        anv_node_raise(node, ROVER_ERR_CAN_ERROR_PASSIVE, 0u, now_ms);
    } else {
        anv_node_lower(node, ROVER_ERR_CAN_ERROR_PASSIVE, 0u, now_ms);
    }
    if (err->state == ACC_CAN_BUS_OFF) {
        anv_node_raise(node, ROVER_ERR_CAN_BUS_OFF, (uint16_t)err->bus_off_count, now_ms);
    } else if (prev == (uint8_t)ACC_CAN_BUS_OFF) {
        anv_node_lower(node, ROVER_ERR_CAN_BUS_OFF, (uint16_t)err->bus_off_count, now_ms);
        anv_node_event(node, ROVER_ERR_CAN_BUS_OFF_RECOVERED, (uint16_t)err->bus_off_count, now_ms);
    }

    uint32_t acks = err->error_type_counts[ACC_CAN_ERRTYPE_ACK];
    if (acks != node->prev_ack_errors) {
        uint32_t delta = acks - node->prev_ack_errors;
        node->prev_ack_errors = acks;
        anv_node_event(node, ROVER_ERR_CAN_ACK_ERROR, (uint16_t)(delta > 0xFFFFu ? 0xFFFFu : delta), now_ms);
    }
}

void anv_node_tick(anv_node_t *node, uint32_t now_ms)
{
    if (node->board->feed_hw_watchdog != NULL) {
        node->board->feed_hw_watchdog(node->board->ctx);
    }

    if (anv_state_get(&node->state) == ANV_STATE_ARMED && !anv_cmd_watchdog_is_fresh(&node->cmd_wd, now_ms)) {
        anv_node_raise(node, ROVER_ERR_CMD_TIMEOUT, 0u, now_ms);
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
        track_can_errors(node, now_ms);
    }

    if (node->diag_on) {
        (void)anv_debug_tick(&node->debug, now_ms);
        node->diag.hold_flash = anv_state_get(&node->state) == ANV_STATE_ARMED;
        anv_diag_tick(&node->diag, now_ms);
    }

    if (anv_rate_sched_due(&node->hb_sched, now_ms)) {
        send_node_hb(node, now_ms);
    }
}
