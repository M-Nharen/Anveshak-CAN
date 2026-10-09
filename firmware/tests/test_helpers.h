/* Shared helpers for the core library's unit tests: a fake board (outputs/watchdog/storage
 * tracked in plain counters) and small frame builders using the generated DBC code, so no
 * test hand-packs a CAN payload either. */
#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "acc_can.h"
#include "acc_can_loopback.h"
#include "anv_board.h"
#include "anv_estop.h"
#include "anv_master.h"
#include "anv_nmt.h"
#include "anv_node.h"
#include "anv_params.h"
#include "ids.h"
#include "params_drive.h"
#include "rover.h"

typedef struct {
    bool outputs_off;
    int outputs_off_calls;
    int hw_watchdog_feeds;
    int enter_bootloader_calls;
    int request_reset_calls;
    uint8_t storage[64];
    size_t storage_len;
    bool storage_present;
} fake_board_ctx_t;

static inline void fake_board_init(fake_board_ctx_t *b)
{
    memset(b, 0, sizeof(*b));
}

static inline void fb_set_outputs_off(void *ctx)
{
    fake_board_ctx_t *b = (fake_board_ctx_t *)ctx;
    b->outputs_off = true;
    b->outputs_off_calls++;
}

static inline void fb_feed_hw_watchdog(void *ctx)
{
    ((fake_board_ctx_t *)ctx)->hw_watchdog_feeds++;
}

static inline void fb_enter_bootloader(void *ctx)
{
    ((fake_board_ctx_t *)ctx)->enter_bootloader_calls++;
}

static inline void fb_request_reset(void *ctx)
{
    ((fake_board_ctx_t *)ctx)->request_reset_calls++;
}

static inline int fb_storage_load(void *ctx, uint8_t *data, size_t len)
{
    fake_board_ctx_t *b = (fake_board_ctx_t *)ctx;
    if (!b->storage_present || len > sizeof(b->storage)) {
        return -1;
    }
    memcpy(data, b->storage, len);
    return 0;
}

static inline int fb_storage_save(void *ctx, const uint8_t *data, size_t len)
{
    fake_board_ctx_t *b = (fake_board_ctx_t *)ctx;
    if (len > sizeof(b->storage)) {
        return -1;
    }
    memcpy(b->storage, data, len);
    b->storage_len = len;
    b->storage_present = true;
    return 0;
}

static inline anv_board_hooks_t fake_board_hooks(fake_board_ctx_t *b)
{
    anv_board_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.ctx = b;
    hooks.set_outputs_off = fb_set_outputs_off;
    hooks.feed_hw_watchdog = fb_feed_hw_watchdog;
    hooks.enter_bootloader = fb_enter_bootloader;
    hooks.request_reset = fb_request_reset;
    hooks.storage_load = fb_storage_load;
    hooks.storage_save = fb_storage_save;
    return hooks;
}

/* Frame builders: thin wrappers over the generated pack functions, with the CAN ID computed
 * generically via ids.h so tests never hand-pack a payload or a frame ID. */

static inline acc_can_frame_t make_estop_frame(void)
{
    acc_can_frame_t frame;
    struct rover_estop_t msg = {.magic = ANV_ESTOP_MAGIC};
    frame.id = ROVER_ESTOP_FRAME_ID;
    frame.dlc = (uint8_t)rover_estop_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

static inline acc_can_frame_t make_estop_release_frame(uint8_t master_node_id, uint8_t master_term_low)
{
    acc_can_frame_t frame;
    struct rover_estop_release_t msg = {.master_node_id = master_node_id, .master_term = master_term_low};
    frame.id = ROVER_ESTOP_RELEASE_FRAME_ID;
    frame.dlc = (uint8_t)rover_estop_release_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

static inline acc_can_frame_t make_sync_frame(uint8_t cycle_counter)
{
    acc_can_frame_t frame;
    struct rover_sync_t msg = {.cycle_counter = cycle_counter};
    frame.id = ROVER_SYNC_FRAME_ID;
    frame.dlc = (uint8_t)rover_sync_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

static inline acc_can_frame_t make_nmt_frame(uint8_t target_node, uint8_t command, uint8_t master_node_id,
                                              uint8_t master_term_low)
{
    acc_can_frame_t frame;
    struct rover_nmt_t msg = {
        .target_node = target_node,
        .command = command,
        .master_node_id = master_node_id,
        .master_term = master_term_low,
    };
    frame.id = ROVER_NMT_FRAME_ID;
    frame.dlc = (uint8_t)rover_nmt_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

/* host = ROVER_NODE_ORIN or ROVER_NODE_RPI. */
static inline acc_can_frame_t make_host_hb_frame(uint8_t host, uint8_t role, uint16_t term)
{
    acc_can_frame_t frame;
    struct rover_host_hb_orin_t msg = {
        .role = role,
        .tiebreak = (host == ROVER_NODE_ORIN) ? 1u : 2u,
        .term = term,
        .believed_master = host,
        .flags = 0u,
        .uptime_s = 0u,
    };
    frame.id = (host == ROVER_NODE_ORIN) ? ROVER_HOST_HB_ORIN_FRAME_ID : ROVER_HOST_HB_RPI_FRAME_ID;
    frame.dlc = (uint8_t)rover_host_hb_orin_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

static inline acc_can_frame_t make_drive_cmd_frame(uint8_t node_id, uint8_t seq, uint8_t sender_host,
                                                    uint8_t term_low, uint8_t mode, int16_t target_a,
                                                    int16_t target_b, uint16_t accel_limit)
{
    acc_can_frame_t frame;
    struct rover_drive1_cmd_t msg = {
        .seq = seq,
        .sender_host = sender_host,
        .term_low = term_low,
        .mode = mode,
        .target_a = target_a,
        .target_b = target_b,
        .accel_limit = accel_limit,
    };
    frame.id = ROVER_CAN_ID(ROVER_CLASS_COMMAND, node_id, 0u);
    frame.dlc = (uint8_t)rover_drive1_cmd_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

static inline acc_can_frame_t make_service_get_frame(uint8_t node_id, uint8_t requester_sub,
                                                       uint8_t transaction_id, uint16_t param_index)
{
    acc_can_frame_t frame;
    struct rover_drive1_service_req_orin_t msg = {
        .opcode = 1u, /* GET */
        .transaction_id = transaction_id,
        .param_index = param_index,
        .value_raw = 0u,
    };
    frame.id = ROVER_CAN_ID(ROVER_CLASS_SERVICE, node_id, requester_sub);
    frame.dlc = (uint8_t)rover_drive1_service_req_orin_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

static inline acc_can_frame_t make_service_set_frame(uint8_t node_id, uint8_t requester_sub,
                                                       uint8_t transaction_id, uint16_t param_index,
                                                       uint32_t value_raw)
{
    acc_can_frame_t frame;
    struct rover_drive1_service_req_orin_t msg = {
        .opcode = 2u, /* SET */
        .transaction_id = transaction_id,
        .param_index = param_index,
        .value_raw = value_raw,
    };
    frame.id = ROVER_CAN_ID(ROVER_CLASS_SERVICE, node_id, requester_sub);
    frame.dlc = (uint8_t)rover_drive1_service_req_orin_pack(frame.data, &msg, sizeof(frame.data));
    return frame;
}

/* Converts a generated ROVER_PARAMS_<TYPE>_TABLE into the core's generic descriptor shape
 * (anv_params.h) -- a plain metadata copy, not byte packing, so this stays within
 * "use generated code" while keeping the core decoupled from any one node type's enum. */
static inline void build_drive_descriptors(anv_param_descriptor_t *out)
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

#define TEST_TXQ_CAP_PER_PRIO 8u

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

    acc_can_frame_t txq_storage[ACC_CAN_PRIO_COUNT * TEST_TXQ_CAP_PER_PRIO];
    anv_node_t node;
} test_rig_t;

/* Sets up a drive node (node_id, e.g. ROVER_NODE_DRIVE1) on a two-port loopback bus shared
 * with a "host" port the test uses to send frames and observe traffic. */
static inline void test_rig_init(test_rig_t *rig, uint8_t node_id, uint32_t now_ms)
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

    anv_node_info_t info = {0};
    anv_node_init(&rig->node, node_id, 1u /* NODE_TYPE_DRIVE */, 1u, &rig->node_driver, rig->txq_storage,
                  TEST_TXQ_CAP_PER_PRIO, &rig->params, &info, &rig->board, now_ms);
    anv_node_boot_complete(&rig->node);
}

/* Sends `frame` from the host port onto the shared bus; the node sees it on its next
 * anv_node_on_rx_frame() poll via test_rig_pump(). */
static inline void test_rig_host_send(test_rig_t *rig, const acc_can_frame_t *frame)
{
    (void)rig->host_driver.send(rig->host_driver.ctx, frame, ACC_CAN_PRIO_SERVICE);
}

/* Delivers every frame currently queued for the node into anv_node_on_rx_frame(). */
static inline void test_rig_pump(test_rig_t *rig, uint32_t now_ms)
{
    acc_can_frame_t frame;
    while (rig->node_driver.receive(rig->node_driver.ctx, &frame)) {
        anv_node_on_rx_frame(&rig->node, &frame, now_ms);
    }
}

/* Sends everything the node has queued for transmission out onto the bus, so the host port
 * can receive() it. */
static inline size_t test_rig_drain(test_rig_t *rig)
{
    return anv_node_drain_tx(&rig->node);
}

/* Convenience: make the current master claim (host HOST_HB + ARM via NMT) so a test can get
 * straight to ARMED without repeating the handshake inline. */
static inline void test_rig_become_master_and_arm(test_rig_t *rig, uint8_t host, uint16_t term,
                                                    uint8_t seq, uint32_t now_ms)
{
    acc_can_frame_t hb = make_host_hb_frame(host, (uint8_t)ANV_HOST_ROLE_MASTER, term);
    test_rig_host_send(rig, &hb);
    test_rig_pump(rig, now_ms);

    acc_can_frame_t cmd = make_drive_cmd_frame(rig->node.node_id, seq, host, (uint8_t)(term & 0x03u), 2u, 0, 0, 0);
    test_rig_host_send(rig, &cmd);
    test_rig_pump(rig, now_ms);

    acc_can_frame_t nmt = make_nmt_frame(rig->node.node_id, ANV_NMT_CMD_ARM, host, (uint8_t)(term & 0xFFu));
    test_rig_host_send(rig, &nmt);
    test_rig_pump(rig, now_ms);
}

#endif /* TEST_HELPERS_H */
