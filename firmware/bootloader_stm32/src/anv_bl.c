#include "anv_bl.h"

#include <string.h>

#include "anv_bl_image.h"
#include "errors.h"
#include "ids.h"
#include "rover.h"

#define RESCUE_RX_ID ROVER_CAN_ID(ROVER_CLASS_BOOTLOADER, ROVER_NODE_UNCONFIGURED, 0u)
#define RESCUE_TX_ID ROVER_CAN_ID(ROVER_CLASS_BOOTLOADER, ROVER_NODE_UNCONFIGURED, 1u)

void anv_bl_can_filters(uint8_t node_id, acc_can_filter_t filters[2])
{
    /* Everything addressed to this node (as in the application), plus the rescue channel's
     * host -> node ID exactly. */
    filters[0].id = (uint16_t)((uint16_t)node_id << 3);
    filters[0].mask = 0x0F8u;
    filters[1].id = RESCUE_RX_ID;
    filters[1].mask = 0x7FFu;
}

static int channel_send(anv_isotp_port_t *self, uint32_t id, const uint8_t *data, uint8_t len)
{
    anv_bl_channel_t *ch = (anv_bl_channel_t *)self;
    acc_can_frame_t frame;
    frame.id = (uint16_t)id;
    frame.dlc = (len > ACC_CAN_MAX_DLC) ? ACC_CAN_MAX_DLC : len;
    memcpy(frame.data, data, frame.dlc);
    if (!anv_txqueue_push(&ch->bl->txq, ACC_CAN_PRIO_SERVICE, &frame)) {
        return ISOTP_RET_NOSPACE;
    }
    return ISOTP_RET_OK;
}

static void channel_init(anv_bl_channel_t *ch, anv_bl_t *bl, uint8_t node_id, bool needs_select)
{
    ch->port.send = channel_send;
    ch->bl = bl;
    ch->rx_id = ROVER_CAN_ID(ROVER_CLASS_BOOTLOADER, node_id, 0u);
    ch->needs_select = needs_select;
    anv_isotp_link_init(&ch->link, &ch->port, ROVER_CAN_ID(ROVER_CLASS_BOOTLOADER, node_id, 1u), ch->tx_buf,
                        sizeof(ch->tx_buf), ch->rx_buf, sizeof(ch->rx_buf));
}

static uint8_t error_severity(uint16_t code)
{
    for (size_t i = 0; i < ROVER_ERROR_TABLE_LEN; i++) {
        if (ROVER_ERROR_TABLE[i].code == code) {
            return (uint8_t)ROVER_ERROR_TABLE[i].severity;
        }
    }
    return (uint8_t)ROVER_SEVERITY_ERROR;
}

/* NODE_EVENT and NODE_HB use drive1's generated layouts for every node, exactly as the core
 * library does (see the design note at the top of firmware/core/src/anv_node.c): the layouts
 * are identical for every node, only the CAN ID differs. */
static void send_event(anv_bl_t *bl, uint16_t code, uint32_t now_ms)
{
    struct rover_drive1_node_event_t msg = {
        .severity = error_severity(code),
        .cleared = 0u,
        .error_code = code,
        .event_seq = bl->event_seq++,
        .detail = 0u,
        .uptime_0_1_s = (uint16_t)((now_ms - bl->start_ms) / 100u),
    };
    acc_can_frame_t frame;
    frame.id = ROVER_CAN_ID(ROVER_CLASS_TELEMETRY, bl->cmd.node_id, 7u);
    int packed = rover_drive1_node_event_pack(frame.data, &msg, sizeof(frame.data));
    if (packed >= 0) {
        frame.dlc = (uint8_t)packed;
        (void)anv_txqueue_push(&bl->txq, ACC_CAN_PRIO_TELEMETRY, &frame);
    }
}

static void send_heartbeat(anv_bl_t *bl, uint32_t now_ms)
{
    acc_can_error_state_t es = {0};
    if (bl->driver->get_error_state != NULL) {
        (void)bl->driver->get_error_state(bl->driver->ctx, &es);
    }
    struct rover_drive1_node_hb_t msg = {
        .state = 5u, /* BOOTLOADER (anv_state_t / NODE_HB STATE) */
        .node_type = bl->cmd.node_type,
        .protocol_version = ANV_BL_PROTOCOL_VERSION,
        .can_error_state = (uint8_t)es.state,
        .bootloader_running = 1u,
        .unsaved_param_changes = 0u,
        .debug_level = 0u,
        .fw_build_id = bl->app_build_id,
        .uptime_s = (uint16_t)((now_ms - bl->start_ms) / 1000u),
    };
    acc_can_frame_t frame;
    frame.id = ROVER_CAN_ID(ROVER_CLASS_HEARTBEAT, bl->cmd.node_id, 0u);
    int packed = rover_drive1_node_hb_pack(frame.data, &msg, sizeof(frame.data));
    if (packed >= 0) {
        frame.dlc = (uint8_t)packed;
        (void)anv_txqueue_push(&bl->txq, ACC_CAN_PRIO_TELEMETRY, &frame);
    }
}

static void refresh_app_status(anv_bl_t *bl)
{
    anv_bl_app_header_t hdr;
    bl->boot_status = anv_bl_app_check(bl->cmd.flash, bl->cmd.node_type, &hdr);
    bl->app_build_id = (bl->boot_status == 0u) ? (uint16_t)hdr.build_id : 0u;
}

static void enter_stay(anv_bl_t *bl, uint32_t now_ms)
{
    bl->mode = ANV_BL_MODE_STAY;
    anv_rate_sched_set_rate(&bl->hb_sched, ANV_BL_HB_RATE_HZ, now_ms);
    send_heartbeat(bl, now_ms); /* announce right away, don't wait a period */
}

void anv_bl_init(anv_bl_t *bl, const anv_bl_config_t *cfg, uint32_t now_ms)
{
    memset(bl, 0, sizeof(*bl));
    bl->driver = cfg->driver;
    anv_bl_cmd_init(&bl->cmd, cfg->flash, cfg->node_id, cfg->node_type, cfg->uid, cfg->flash_size_kb);
    bl->cmd.feed_watchdog = cfg->feed_watchdog;
    bl->cmd.feed_ctx = cfg->feed_ctx;
    anv_txqueue_init(&bl->txq, bl->txq_storage, ANV_BL_TXQ_PER_PRIO);

    bool unconfigured = (cfg->node_id == ROVER_NODE_UNCONFIGURED);
    channel_init(&bl->own, bl, cfg->node_id, unconfigured);
    bl->has_rescue = !unconfigured;
    if (bl->has_rescue) {
        channel_init(&bl->rescue, bl, ROVER_NODE_UNCONFIGURED, true);
    }

    bl->start_ms = now_ms;
    bl->last_ms = now_ms;
    anv_isotp_set_time_ms(now_ms);
    anv_rate_sched_init(&bl->hb_sched, 0u, now_ms);
    refresh_app_status(bl);

    bl->mode = ANV_BL_MODE_LISTEN;
    if (cfg->stay_flag) {
        enter_stay(bl, now_ms);
    }
}

void anv_bl_on_rx_frame(anv_bl_t *bl, const acc_can_frame_t *frame)
{
    if (bl->mode == ANV_BL_MODE_JUMP || bl->mode == ANV_BL_MODE_RESET) {
        return;
    }
    uint8_t dlc = (frame->dlc > ACC_CAN_MAX_DLC) ? ACC_CAN_MAX_DLC : frame->dlc;
    if (frame->id == bl->own.rx_id) {
        isotp_on_can_message(&bl->own.link, frame->data, dlc);
    } else if (bl->has_rescue && frame->id == bl->rescue.rx_id) {
        isotp_on_can_message(&bl->rescue.link, frame->data, dlc);
    }
}

static void serve_channel(anv_bl_t *bl, anv_bl_channel_t *ch, uint32_t now_ms)
{
    uint32_t req_len = 0u;
    if (isotp_receive(&ch->link, bl->req, sizeof(bl->req), &req_len) != ISOTP_RET_OK || req_len == 0u) {
        return;
    }
    /* On a shared channel nothing but SELECT is answered until this board is selected, so
     * two unconfigured boards never answer (or flow-control) the same request. */
    if (ch->needs_select && !bl->cmd.selected && bl->req[0] != ANV_BL_CMD_SELECT) {
        return;
    }
    size_t resp_len = anv_bl_cmd_handle(&bl->cmd, bl->req, req_len, bl->resp);
    if (resp_len == 0u) {
        return;
    }
    (void)isotp_send(&ch->link, bl->resp, (uint32_t)resp_len);

    if (bl->mode == ANV_BL_MODE_LISTEN) {
        enter_stay(bl, now_ms); /* the host found us in the window: stay */
    }
    if (bl->req[0] == ANV_BL_CMD_COMMIT || bl->req[0] == ANV_BL_CMD_ABORT || bl->req[0] == ANV_BL_CMD_ERASE) {
        refresh_app_status(bl);
    }
}

static bool channel_idle(const anv_bl_channel_t *ch)
{
    return ch->link.send_status != ISOTP_SEND_STATUS_INPROGRESS;
}

anv_bl_mode_t anv_bl_poll(anv_bl_t *bl, uint32_t now_ms)
{
    if (bl->mode == ANV_BL_MODE_JUMP || bl->mode == ANV_BL_MODE_RESET) {
        return bl->mode;
    }
    bl->last_ms = now_ms;
    anv_isotp_set_time_ms(now_ms);

    isotp_poll(&bl->own.link);
    if (bl->has_rescue) {
        isotp_poll(&bl->rescue.link);
    }
    serve_channel(bl, &bl->own, now_ms);
    if (bl->has_rescue) {
        serve_channel(bl, &bl->rescue, now_ms);
    }

    if (bl->mode == ANV_BL_MODE_LISTEN && (now_ms - bl->start_ms) >= ANV_BL_LISTEN_MS) {
        refresh_app_status(bl);
        if (bl->boot_status == 0u) {
            bl->mode = ANV_BL_MODE_JUMP;
            return bl->mode;
        }
        enter_stay(bl, now_ms);
        send_event(bl, bl->boot_status, now_ms);
    }

    if (bl->mode == ANV_BL_MODE_STAY && anv_rate_sched_due(&bl->hb_sched, now_ms)) {
        send_heartbeat(bl, now_ms);
    }

    (void)anv_txqueue_drain(&bl->txq, bl->driver);

    if (bl->cmd.boot_requested && !bl->reset_pending && channel_idle(&bl->own) &&
        (!bl->has_rescue || channel_idle(&bl->rescue)) && anv_txqueue_is_empty(&bl->txq)) {
        bl->reset_pending = true;
        bl->reset_at_ms = now_ms + ANV_BL_RESET_DELAY_MS;
    }
    if (bl->reset_pending && (int32_t)(now_ms - bl->reset_at_ms) >= 0) {
        bl->mode = ANV_BL_MODE_RESET;
    }
    return bl->mode;
}

void anv_bl_refuse_jump(anv_bl_t *bl, uint16_t error_code, uint32_t now_ms)
{
    bl->boot_status = error_code;
    bl->app_build_id = 0u;
    enter_stay(bl, now_ms);
    send_event(bl, error_code, now_ms);
}
