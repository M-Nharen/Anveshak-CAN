/* Helpers for the diagnostics tests (A4): a fake flash log that outlives a "reset", frame
 * builders for DEBUG_LEVEL / LOG_READ, and a decoder for NODE_EVENT frames. Kept apart from
 * test_helpers.h so that file stays untouched. */
#ifndef TEST_DIAG_HELPERS_H
#define TEST_DIAG_HELPERS_H

#include "test_helpers.h"

#include "errors.h"

/* Board-side state that survives a simulated reset (flash, no-init RAM). A reset in a test is
 * test_rig_init() again with this left alone; "power loss" is diag_hw_wipe(). */
typedef struct {
    uint8_t log[ANV_DIAG_BLOB_SIZE];
    bool log_present;
    bool log_fail; /* make log_save fail */
    int log_saves;
    bool crash_present;
    anv_crash_record_t crash;
    uint8_t reset_cause;
    uint32_t self_test_mask;
    uint32_t now_ms;
} diag_hw_t;

static diag_hw_t g_hw;

static inline void diag_hw_wipe(void)
{
    memset(&g_hw, 0, sizeof(g_hw));
}

static inline int dh_log_load(void *ctx, uint8_t *data, size_t len)
{
    (void)ctx;
    if (!g_hw.log_present || len > sizeof(g_hw.log)) {
        return -1;
    }
    memcpy(data, g_hw.log, len);
    return 0;
}

static inline int dh_log_save(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (g_hw.log_fail || len > sizeof(g_hw.log)) {
        return -1;
    }
    memcpy(g_hw.log, data, len);
    g_hw.log_present = true;
    g_hw.log_saves++;
    return 0;
}

static inline bool dh_take_crash(void *ctx, anv_crash_record_t *out)
{
    (void)ctx;
    if (!g_hw.crash_present) {
        return false;
    }
    *out = g_hw.crash;
    g_hw.crash_present = false;
    return true;
}

static inline uint8_t dh_reset_cause(void *ctx)
{
    (void)ctx;
    return g_hw.reset_cause;
}

static inline uint32_t dh_self_test(void *ctx)
{
    (void)ctx;
    return g_hw.self_test_mask;
}

static inline uint32_t dh_now_ms(void *ctx)
{
    (void)ctx;
    return g_hw.now_ms;
}

/* A rig as after a reset: new RAM, same flash. Diagnostics started with `debug_default`. */
static inline void diag_rig_boot(test_rig_t *rig, uint8_t node_id, uint32_t now_ms, uint8_t debug_default)
{
    test_rig_init(rig, node_id, now_ms);
    rig->board.log_load = dh_log_load;
    rig->board.log_save = dh_log_save;
    rig->board.take_crash_record = dh_take_crash;
    rig->board.reset_cause = dh_reset_cause;
    rig->board.self_test = dh_self_test;
    rig->board.now_ms = dh_now_ms;
    g_hw.now_ms = now_ms;
    (void)anv_node_diag_start(&rig->node, debug_default, now_ms);
}

static inline acc_can_frame_t make_debug_level_frame(uint8_t target, uint8_t level, uint8_t mask,
                                                       uint16_t minutes, uint8_t master, uint8_t term)
{
    acc_can_frame_t frame;
    struct rover_debug_level_t msg = {
        .target_node = target,
        .level = level,
        .category_mask = mask,
        .auto_off_timer_min = minutes,
        .master_node_id = master,
        .master_term = term,
    };
    frame.id = ROVER_DEBUG_LEVEL_FRAME_ID;
    frame.dlc = (uint8_t)rover_debug_level_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

static inline acc_can_frame_t make_service_frame(uint8_t node_id, uint8_t requester_sub, uint8_t txn,
                                                   uint8_t opcode, uint16_t index, uint32_t value)
{
    acc_can_frame_t frame;
    struct rover_drive1_service_req_orin_t msg = {
        .opcode = opcode,
        .transaction_id = txn,
        .param_index = index,
        .value_raw = value,
    };
    frame.id = ROVER_CAN_ID(ROVER_CLASS_SERVICE, node_id, requester_sub);
    frame.dlc = (uint8_t)rover_drive1_service_req_orin_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

typedef struct {
    uint8_t severity;
    bool cleared;
    uint16_t code;
    uint8_t seq;
    uint16_t detail;
    uint16_t uptime_ds;
} seen_event_t;

#define SEEN_MAX 64u

/* Sends whatever the node has queued and returns the NODE_EVENT frames the host port receives
 * (every other frame is dropped; use drain_all_frames for those). */
static inline size_t collect_events(test_rig_t *rig, seen_event_t *out, size_t max)
{
    size_t n = 0;
    acc_can_frame_t frame;
    (void)test_rig_drain(rig);
    while (rig->host_driver.receive(rig->host_driver.ctx, &frame)) {
        if (frame.id != ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, rig->node.node_id, 7u) || n >= max) {
            continue;
        }
        struct rover_drive1_node_event_t ev;
        if (rover_drive1_node_event_unpack(&ev, frame.data, frame.dlc) != 0) {
            continue;
        }
        out[n].severity = ev.severity;
        out[n].cleared = ev.cleared != 0u;
        out[n].code = ev.error_code;
        out[n].seq = ev.event_seq;
        out[n].detail = ev.detail;
        out[n].uptime_ds = ev.uptime_0_1_s;
        n++;
    }
    return n;
}

/* Drains the node and returns how many frames with this CAN ID the host saw. */
static inline size_t count_frames_with_id(test_rig_t *rig, uint32_t id)
{
    size_t n = 0;
    acc_can_frame_t frame;
    (void)test_rig_drain(rig);
    while (rig->host_driver.receive(rig->host_driver.ctx, &frame)) {
        if (frame.id == id) {
            n++;
        }
    }
    return n;
}

#endif /* TEST_DIAG_HELPERS_H */
