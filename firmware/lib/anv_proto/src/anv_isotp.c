#include "anv_isotp.h"

#include <string.h>

/* Protocol control information: high nibble of the first byte. */
#define PCI_SF 0x0u
#define PCI_FF 0x1u
#define PCI_CF 0x2u
#define PCI_FC 0x3u

#define FS_CTS 0x0u
#define FS_WAIT 0x1u
#define FS_OVERFLOW 0x2u

static bool time_reached(uint32_t now_us, uint32_t at_us)
{
    return (int32_t)(now_us - at_us) >= 0;
}

static uint32_t timeout_us(const anv_isotp_t *t)
{
    return (uint32_t)t->cfg.timeout_ms * 1000u;
}

uint32_t anv_isotp_st_min_us(uint8_t st_min)
{
    if (st_min <= 0x7Fu) {
        return (uint32_t)st_min * 1000u;
    }
    if (st_min >= 0xF1u && st_min <= 0xF9u) {
        return (uint32_t)(st_min - 0xF0u) * 100u;
    }
    return 127000u;
}

void anv_isotp_config_default(anv_isotp_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = ANV_TRANSFER_PORT;
    cfg->priority = ANV_TRANSFER_PRIORITY;
    cfg->block_size = ANV_ISOTP_DEFAULT_BLOCK_SIZE;
    cfg->st_min = ANV_ISOTP_DEFAULT_ST_MIN;
    cfg->timeout_ms = ANV_ISOTP_DEFAULT_TIMEOUT_MS;
    cfg->max_wait = ANV_ISOTP_DEFAULT_MAX_WAIT;
    cfg->padding = ANV_ISOTP_DEFAULT_PADDING;
}

anv_isotp_result_t anv_isotp_init(anv_isotp_t *t, const anv_isotp_config_t *cfg)
{
    memset(t, 0, sizeof(*t));
    const anv_proto_addr_t a = {cfg->priority, cfg->peer, cfg->port, cfg->self};
    if (cfg->send == NULL || cfg->on_rx == NULL || cfg->rx_buf == NULL || cfg->rx_buf_size == 0u ||
        !anv_proto_addr_valid(&a) || cfg->timeout_ms == 0u) {
        return ANV_ISOTP_ERR_ARG;
    }
    t->cfg = *cfg;
    return ANV_ISOTP_OK;
}

/* Every frame of this channel goes to the peer, padded to 8 bytes. */
static bool send_payload(anv_isotp_t *t, const uint8_t *p, uint8_t n)
{
    anv_link_frame_t f;
    f.id = anv_id_make(t->cfg.priority, t->cfg.peer, t->cfg.port, t->cfg.self);
    f.len = ANV_LINK_MAX_DLC;
    memset(f.data, t->cfg.padding, sizeof(f.data));
    memcpy(f.data, p, n);
    return t->cfg.send(t->cfg.user, &f);
}

/* ---- sending ------------------------------------------------------------------------- */

static void tx_finish(anv_isotp_t *t, anv_isotp_result_t r)
{
    t->tx_state = ANV_ISOTP_TX_IDLE;
    t->tx_data = NULL;
    if (r == ANV_ISOTP_OK) {
        t->stats.tx_done++;
    } else {
        t->stats.tx_failed++;
    }
    if (t->cfg.on_tx_done != NULL) {
        t->cfg.on_tx_done(t->cfg.user, r);
    }
}

static void tx_send_first(anv_isotp_t *t, uint32_t now_us)
{
    uint8_t p[8];
    if (t->tx_len <= ANV_ISOTP_SF_MAX) {
        p[0] = (uint8_t)((PCI_SF << 4) | t->tx_len);
        memcpy(p + 1, t->tx_data, t->tx_len);
        if (send_payload(t, p, (uint8_t)(1u + t->tx_len))) {
            tx_finish(t, ANV_ISOTP_OK);
        }
        return;
    }
    p[0] = (uint8_t)((PCI_FF << 4) | (t->tx_len >> 8));
    p[1] = (uint8_t)(t->tx_len & 0xFFu);
    memcpy(p + 2, t->tx_data, 6);
    if (send_payload(t, p, 8)) {
        t->tx_off = 6;
        t->tx_sn = 1;
        t->tx_waits = 0;
        t->tx_state = ANV_ISOTP_TX_WAIT_FC;
        t->tx_deadline_us = now_us + timeout_us(t);
    }
}

static void tx_send_cfs(anv_isotp_t *t, uint32_t now_us)
{
    for (uint8_t burst = 0; burst < ANV_ISOTP_MAX_BURST; burst++) {
        if (!time_reached(now_us, t->tx_next_us)) {
            return;
        }
        const uint16_t left = (uint16_t)(t->tx_len - t->tx_off);
        const uint8_t n = (uint8_t)(left > 7u ? 7u : left);
        uint8_t p[8];
        p[0] = (uint8_t)((PCI_CF << 4) | t->tx_sn);
        memcpy(p + 1, t->tx_data + t->tx_off, n);
        if (!send_payload(t, p, (uint8_t)(1u + n))) {
            return; /* link full: retry next poll, the stall deadline stays */
        }
        t->tx_off = (uint16_t)(t->tx_off + n);
        t->tx_sn = (uint8_t)((t->tx_sn + 1u) & 0x0Fu);
        t->tx_deadline_us = now_us + timeout_us(t);
        if (t->tx_off >= t->tx_len) {
            tx_finish(t, ANV_ISOTP_OK);
            return;
        }
        if (t->tx_bs != 0u && --t->tx_bs_left == 0u) {
            t->tx_state = ANV_ISOTP_TX_WAIT_FC;
            return;
        }
        t->tx_next_us = now_us + t->tx_st_us;
        if (t->tx_st_us != 0u) {
            return; /* STmin > 0: never two frames in one poll */
        }
    }
}

anv_isotp_result_t anv_isotp_send(anv_isotp_t *t, const uint8_t *data, uint16_t len, uint32_t now_us)
{
    if (data == NULL || len == 0u || len > ANV_ISOTP_MAX_LEN) {
        return ANV_ISOTP_ERR_ARG;
    }
    if (t->tx_state != ANV_ISOTP_TX_IDLE) {
        return ANV_ISOTP_ERR_BUSY;
    }
    t->tx_data = data;
    t->tx_len = len;
    t->tx_off = 0;
    t->tx_state = ANV_ISOTP_TX_SEND_FIRST;
    t->tx_deadline_us = now_us + timeout_us(t);
    tx_send_first(t, now_us);
    return ANV_ISOTP_OK;
}

static void on_flow_control(anv_isotp_t *t, const anv_link_frame_t *f, uint32_t now_us)
{
    if (t->tx_state != ANV_ISOTP_TX_WAIT_FC) {
        t->stats.rx_stray++;
        return;
    }
    if (f->len < 3u) {
        tx_finish(t, ANV_ISOTP_ERR_BAD_FRAME);
        return;
    }
    switch (f->data[0] & 0x0Fu) {
    case FS_CTS:
        t->tx_bs = f->data[1];
        t->tx_bs_left = f->data[1];
        t->tx_st_us = anv_isotp_st_min_us(f->data[2]);
        t->tx_waits = 0;
        t->tx_next_us = now_us; /* STmin is between consecutive frames, not after flow control */
        t->tx_deadline_us = now_us + timeout_us(t);
        t->tx_state = ANV_ISOTP_TX_SEND_CF;
        tx_send_cfs(t, now_us);
        break;
    case FS_WAIT:
        if (++t->tx_waits > t->cfg.max_wait) {
            tx_finish(t, ANV_ISOTP_ERR_WAIT_LIMIT);
        } else {
            t->tx_deadline_us = now_us + timeout_us(t);
        }
        break;
    case FS_OVERFLOW:
        tx_finish(t, ANV_ISOTP_ERR_OVERFLOW);
        break;
    default:
        tx_finish(t, ANV_ISOTP_ERR_BAD_FRAME);
        break;
    }
}

/* ---- receiving ----------------------------------------------------------------------- */

static void rx_fail(anv_isotp_t *t, anv_isotp_result_t r)
{
    t->rx_active = false;
    t->stats.rx_failed++;
    if (r == ANV_ISOTP_ERR_WRONG_SN) {
        t->stats.rx_wrong_sn++;
    }
    if (t->cfg.on_rx_error != NULL) {
        t->cfg.on_rx_error(t->cfg.user, r);
    }
}

static void rx_deliver(anv_isotp_t *t, uint16_t len)
{
    t->rx_active = false;
    t->stats.rx_done++;
    t->cfg.on_rx(t->cfg.user, t->cfg.rx_buf, len);
}

static void rx_send_fc(anv_isotp_t *t, uint8_t status)
{
    const uint8_t p[3] = {(uint8_t)((PCI_FC << 4) | status), t->cfg.block_size, t->cfg.st_min};
    t->rx_fc_status = status;
    t->rx_fc_pending = !send_payload(t, p, sizeof p);
}

static void on_single(anv_isotp_t *t, const anv_link_frame_t *f)
{
    const uint8_t len = f->data[0] & 0x0Fu;
    if (t->rx_active) {
        rx_fail(t, ANV_ISOTP_ERR_INTERRUPTED);
    }
    if (len == 0u || len > ANV_ISOTP_SF_MAX || len + 1u > f->len) {
        rx_fail(t, ANV_ISOTP_ERR_BAD_FRAME);
        return;
    }
    if (len > t->cfg.rx_buf_size) {
        rx_fail(t, ANV_ISOTP_ERR_OVERFLOW);
        return;
    }
    memcpy(t->cfg.rx_buf, f->data + 1, len);
    rx_deliver(t, len);
}

static void on_first(anv_isotp_t *t, const anv_link_frame_t *f, uint32_t now_us)
{
    const uint16_t len = (uint16_t)(((f->data[0] & 0x0Fu) << 8) | f->data[1]);
    if (t->rx_active) {
        rx_fail(t, ANV_ISOTP_ERR_INTERRUPTED);
    }
    if (f->len < 8u || len <= ANV_ISOTP_SF_MAX) {
        rx_fail(t, ANV_ISOTP_ERR_BAD_FRAME);
        return;
    }
    if (len > t->cfg.rx_buf_size) {
        rx_send_fc(t, FS_OVERFLOW);
        rx_fail(t, ANV_ISOTP_ERR_OVERFLOW);
        return;
    }
    memcpy(t->cfg.rx_buf, f->data + 2, 6);
    t->rx_len = len;
    t->rx_got = 6;
    t->rx_sn = 1;
    t->rx_bs_left = t->cfg.block_size;
    t->rx_active = true;
    t->rx_deadline_us = now_us + timeout_us(t);
    rx_send_fc(t, FS_CTS);
}

static void on_consecutive(anv_isotp_t *t, const anv_link_frame_t *f, uint32_t now_us)
{
    if (!t->rx_active) {
        t->stats.rx_stray++;
        return;
    }
    if ((f->data[0] & 0x0Fu) != t->rx_sn) {
        /* Lost, repeated or reordered: the rest can't be trusted. The sender finds out by its
         * flow-control timeout (block_size > 0) or by the protocol on top. */
        rx_fail(t, ANV_ISOTP_ERR_WRONG_SN);
        return;
    }
    const uint16_t left = (uint16_t)(t->rx_len - t->rx_got);
    const uint8_t n = (uint8_t)(left > 7u ? 7u : left);
    if (f->len < 1u + n) {
        rx_fail(t, ANV_ISOTP_ERR_BAD_FRAME);
        return;
    }
    memcpy(t->cfg.rx_buf + t->rx_got, f->data + 1, n);
    t->rx_got = (uint16_t)(t->rx_got + n);
    t->rx_sn = (uint8_t)((t->rx_sn + 1u) & 0x0Fu);
    t->rx_deadline_us = now_us + timeout_us(t);
    if (t->rx_got >= t->rx_len) {
        rx_deliver(t, t->rx_len);
        return;
    }
    if (t->cfg.block_size != 0u && --t->rx_bs_left == 0u) {
        t->rx_bs_left = t->cfg.block_size;
        rx_send_fc(t, FS_CTS);
    }
}

bool anv_isotp_on_frame(anv_isotp_t *t, const anv_link_frame_t *f, uint32_t now_us)
{
    if (anv_id_port(f->id) != t->cfg.port || anv_id_src(f->id) != t->cfg.peer ||
        anv_id_dst(f->id) != t->cfg.self) {
        return false;
    }
    if (f->len < 1u) {
        return true;
    }
    switch (f->data[0] >> 4) {
    case PCI_SF:
        on_single(t, f);
        break;
    case PCI_FF:
        on_first(t, f, now_us);
        break;
    case PCI_CF:
        on_consecutive(t, f, now_us);
        break;
    case PCI_FC:
        on_flow_control(t, f, now_us);
        break;
    default:
        break; /* not ISO-TP on classic CAN: ignore */
    }
    return true;
}

void anv_isotp_poll(anv_isotp_t *t, uint32_t now_us)
{
    if (t->rx_fc_pending) {
        rx_send_fc(t, t->rx_fc_status);
    }
    if (t->rx_active && time_reached(now_us, t->rx_deadline_us)) {
        rx_fail(t, ANV_ISOTP_ERR_TIMEOUT_CF);
    }

    switch (t->tx_state) {
    case ANV_ISOTP_TX_SEND_FIRST:
        tx_send_first(t, now_us);
        if (t->tx_state == ANV_ISOTP_TX_SEND_FIRST && time_reached(now_us, t->tx_deadline_us)) {
            tx_finish(t, ANV_ISOTP_ERR_TX_STALLED);
        }
        break;
    case ANV_ISOTP_TX_WAIT_FC:
        if (time_reached(now_us, t->tx_deadline_us)) {
            tx_finish(t, ANV_ISOTP_ERR_TIMEOUT_FC);
        }
        break;
    case ANV_ISOTP_TX_SEND_CF:
        tx_send_cfs(t, now_us);
        if (t->tx_state == ANV_ISOTP_TX_SEND_CF && time_reached(now_us, t->tx_deadline_us)) {
            tx_finish(t, ANV_ISOTP_ERR_TX_STALLED);
        }
        break;
    case ANV_ISOTP_TX_IDLE:
    default:
        break;
    }
}

void anv_isotp_abort(anv_isotp_t *t)
{
    if (t->tx_state != ANV_ISOTP_TX_IDLE) {
        tx_finish(t, ANV_ISOTP_ERR_ABORTED);
    }
    if (t->rx_active) {
        rx_fail(t, ANV_ISOTP_ERR_ABORTED);
    }
    t->rx_fc_pending = false;
}

bool anv_isotp_tx_busy(const anv_isotp_t *t)
{
    return t->tx_state != ANV_ISOTP_TX_IDLE;
}

bool anv_isotp_rx_busy(const anv_isotp_t *t)
{
    return t->rx_active;
}

const char *anv_isotp_result_name(anv_isotp_result_t r)
{
    switch (r) {
    case ANV_ISOTP_OK: return "ok";
    case ANV_ISOTP_ERR_ARG: return "bad argument";
    case ANV_ISOTP_ERR_BUSY: return "busy";
    case ANV_ISOTP_ERR_TIMEOUT_FC: return "no flow control";
    case ANV_ISOTP_ERR_TIMEOUT_CF: return "consecutive frame timeout";
    case ANV_ISOTP_ERR_TX_STALLED: return "link refused frames";
    case ANV_ISOTP_ERR_WRONG_SN: return "wrong sequence number";
    case ANV_ISOTP_ERR_OVERFLOW: return "too long for the receiver";
    case ANV_ISOTP_ERR_WAIT_LIMIT: return "too many waits";
    case ANV_ISOTP_ERR_INTERRUPTED: return "interrupted by a new transfer";
    case ANV_ISOTP_ERR_BAD_FRAME: return "malformed frame";
    case ANV_ISOTP_ERR_ABORTED: return "aborted";
    }
    return "?";
}
