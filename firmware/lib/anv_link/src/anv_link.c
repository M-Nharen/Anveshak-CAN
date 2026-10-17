#include "anv_link.h"

#include <string.h>

void anv_link_config_default(anv_link_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->bitrate = ANV_LINK_DEFAULT_BITRATE;
    cfg->auto_recover = true;
    cfg->recover_retry_ms = ANV_LINK_DEFAULT_RECOVER_RETRY_MS;
}

void anv_link_init(anv_link_t *link, const anv_link_ops_t *ops, void *ctx)
{
    memset(link, 0, sizeof(*link));
    link->ops = ops;
    link->ctx = ctx;
    anv_link_config_default(&link->cfg);
}

static bool filters_valid(const anv_link_filter_t *filters, uint8_t n)
{
    return n <= ANV_LINK_MAX_FILTERS && (n == 0u || filters != NULL);
}

int anv_link_start(anv_link_t *link, const anv_link_config_t *cfg)
{
    if (link->ops == NULL || cfg == NULL || cfg->bitrate == 0u || !filters_valid(cfg->filters, cfg->n_filters)) {
        return ANV_LINK_ERR_ARG;
    }
    link->cfg = *cfg;
    memset(&link->stats, 0, sizeof(link->stats));
    link->recovering = false;
    const int rc = link->ops->start(link->ctx, &link->cfg);
    link->started = rc == ANV_LINK_OK;
    return rc;
}

int anv_link_stop(anv_link_t *link)
{
    if (!link->started) {
        return ANV_LINK_OK;
    }
    link->started = false;
    return link->ops->stop != NULL ? link->ops->stop(link->ctx) : ANV_LINK_OK;
}

int anv_link_set_filters(anv_link_t *link, const anv_link_filter_t *filters, uint8_t n)
{
    if (!filters_valid(filters, n)) {
        return ANV_LINK_ERR_ARG;
    }
    for (uint8_t i = 0; i < n; i++) {
        link->cfg.filters[i] = filters[i];
    }
    link->cfg.n_filters = n;
    if (link->started && link->ops->set_filters != NULL) {
        return link->ops->set_filters(link->ctx, link->cfg.filters, n);
    }
    return ANV_LINK_OK;
}

bool anv_link_filters_accept(const anv_link_filter_t *filters, uint8_t n, uint32_t id)
{
    if (n == 0u) {
        return true;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (((id ^ filters[i].id) & filters[i].mask & ANV_LINK_EXT_ID_MASK) == 0u) {
            return true;
        }
    }
    return false;
}

int anv_link_send(anv_link_t *link, const anv_link_frame_t *frame)
{
    if (frame == NULL || frame->len > ANV_LINK_MAX_DLC || frame->id > ANV_LINK_EXT_ID_MASK) {
        return ANV_LINK_ERR_ARG;
    }
    if (!link->started || link->stats.state == ANV_LINK_STATE_BUS_OFF) {
        link->stats.tx_rejected++;
        return ANV_LINK_ERR_NOT_READY;
    }
    const int rc = link->ops->send(link->ctx, frame);
    if (rc == ANV_LINK_OK) {
        link->stats.tx_frames++;
    } else {
        link->stats.tx_rejected++;
    }
    return rc;
}

bool anv_link_receive(anv_link_t *link, anv_link_frame_t *frame)
{
    if (!link->started) {
        return false;
    }
    while (link->ops->receive(link->ctx, frame)) {
        if (anv_link_filters_accept(link->cfg.filters, link->cfg.n_filters, frame->id)) {
            link->stats.rx_frames++;
            return true;
        }
        link->stats.rx_filtered++;
    }
    return false;
}

int anv_link_recover(anv_link_t *link)
{
    if (!link->started) {
        return ANV_LINK_ERR_NOT_READY;
    }
    link->recovering = true;
    return link->ops->recover != NULL ? link->ops->recover(link->ctx) : ANV_LINK_ERR_BACKEND;
}

uint32_t anv_link_poll(anv_link_t *link, uint32_t now_ms)
{
    if (!link->started || link->ops->get_status == NULL) {
        return 0;
    }
    anv_link_hw_status_t hw;
    if (link->ops->get_status(link->ctx, &hw) != ANV_LINK_OK) {
        return 0;
    }

    uint32_t ev = 0;
    anv_link_stats_t *st = &link->stats;
    if (hw.rx_overflow != st->rx_overflow) {
        ev |= ANV_LINK_EV_RX_OVERFLOW;
    }
    if (hw.tx_failed != st->tx_failed) {
        ev |= ANV_LINK_EV_TX_FAILED;
    }
    st->rx_overflow = hw.rx_overflow;
    st->tx_failed = hw.tx_failed;
    st->tec = hw.tec;
    st->rec = hw.rec;

    const anv_link_state_t prev = st->state;
    st->state = hw.state;

    if (hw.state == ANV_LINK_STATE_BUS_OFF) {
        if (prev != ANV_LINK_STATE_BUS_OFF) {
            st->bus_off_count++;
            ev |= ANV_LINK_EV_BUS_OFF;
            link->recovering = false;
        }
        /* Bus-off never ends by itself on TWAI; retrying covers a recovery the controller
         * refused (e.g. asked while it was still settling). */
        if (link->cfg.auto_recover &&
            (!link->recovering || (int32_t)(now_ms - link->recover_at_ms) >= 0)) {
            (void)anv_link_recover(link);
            link->recover_at_ms = now_ms + link->cfg.recover_retry_ms;
        }
    } else {
        if (prev == ANV_LINK_STATE_BUS_OFF) {
            st->recoveries++;
            ev |= ANV_LINK_EV_RECOVERED;
            link->recovering = false;
        }
        if (hw.state == ANV_LINK_STATE_PASSIVE && prev != ANV_LINK_STATE_PASSIVE) {
            ev |= ANV_LINK_EV_PASSIVE;
        }
    }
    return ev;
}

const anv_link_stats_t *anv_link_stats(const anv_link_t *link)
{
    return &link->stats;
}
