#include "anv_link_loopback.h"

#include <string.h>

static void deliver(anv_loopback_port_t *to, const anv_link_frame_t *frame)
{
    if (!to->started) {
        return;
    }
    if (to->rx_count >= to->rx_capacity) {
        to->status.rx_overflow++;
        return;
    }
    const uint16_t tail = (uint16_t)((to->rx_head + to->rx_count) % ANV_LOOPBACK_RX_CAPACITY);
    to->rx[tail] = *frame;
    to->rx_count++;
}

static int lb_start(void *ctx, const anv_link_config_t *cfg)
{
    (void)cfg; /* the front end filters; bitrate means nothing here */
    anv_loopback_port_t *port = (anv_loopback_port_t *)ctx;
    if (port->bus == NULL) {
        return ANV_LINK_ERR_BACKEND;
    }
    port->rx_head = 0;
    port->rx_count = 0;
    memset(&port->status, 0, sizeof(port->status));
    port->started = true;
    return ANV_LINK_OK;
}

static int lb_stop(void *ctx)
{
    ((anv_loopback_port_t *)ctx)->started = false;
    return ANV_LINK_OK;
}

static int lb_set_filters(void *ctx, const anv_link_filter_t *filters, uint8_t n)
{
    (void)ctx;
    (void)filters;
    (void)n;
    return ANV_LINK_OK;
}

static int lb_send(void *ctx, const anv_link_frame_t *frame)
{
    anv_loopback_port_t *from = (anv_loopback_port_t *)ctx;
    if (!from->started || from->status.state == ANV_LINK_STATE_BUS_OFF) {
        return ANV_LINK_ERR_NOT_READY;
    }
    anv_link_frame_t f = *frame;
    anv_loopback_bus_t *bus = from->bus;
    if (bus->tap != NULL && !bus->tap(bus->tap_user, from, &f)) {
        return ANV_LINK_OK; /* lost on the wire: the sender can't tell */
    }
    for (uint8_t i = 0; i < bus->count; i++) {
        if (bus->ports[i] != from) {
            deliver(bus->ports[i], &f);
        }
    }
    return ANV_LINK_OK;
}

static bool lb_receive(void *ctx, anv_link_frame_t *frame)
{
    anv_loopback_port_t *port = (anv_loopback_port_t *)ctx;
    if (port->rx_count == 0u) {
        return false;
    }
    *frame = port->rx[port->rx_head];
    port->rx_head = (uint16_t)((port->rx_head + 1u) % ANV_LOOPBACK_RX_CAPACITY);
    port->rx_count--;
    return true;
}

static int lb_get_status(void *ctx, anv_link_hw_status_t *status)
{
    *status = ((anv_loopback_port_t *)ctx)->status;
    return ANV_LINK_OK;
}

static int lb_recover(void *ctx)
{
    anv_loopback_port_t *port = (anv_loopback_port_t *)ctx;
    port->recover_requests++;
    if (port->status.state == ANV_LINK_STATE_BUS_OFF && !port->hold_bus_off) {
        port->status.state = ANV_LINK_STATE_ACTIVE;
        port->status.tec = 0;
        port->status.rec = 0;
    }
    return ANV_LINK_OK;
}

static const anv_link_ops_t s_ops = {
    .start = lb_start,
    .stop = lb_stop,
    .set_filters = lb_set_filters,
    .send = lb_send,
    .receive = lb_receive,
    .get_status = lb_get_status,
    .recover = lb_recover,
};

const anv_link_ops_t *anv_loopback_ops(void)
{
    return &s_ops;
}

void anv_loopback_bus_init(anv_loopback_bus_t *bus)
{
    memset(bus, 0, sizeof(*bus));
}

void anv_loopback_set_tap(anv_loopback_bus_t *bus, anv_loopback_tap_t tap, void *user)
{
    bus->tap = tap;
    bus->tap_user = user;
}

bool anv_loopback_attach(anv_loopback_bus_t *bus, anv_loopback_port_t *port)
{
    if (bus->count >= ANV_LOOPBACK_MAX_PORTS) {
        return false;
    }
    memset(port, 0, sizeof(*port));
    port->bus = bus;
    port->rx_capacity = ANV_LOOPBACK_RX_CAPACITY;
    bus->ports[bus->count++] = port;
    return true;
}

void anv_loopback_inject(anv_loopback_bus_t *bus, const anv_link_frame_t *frame)
{
    for (uint8_t i = 0; i < bus->count; i++) {
        deliver(bus->ports[i], frame);
    }
}

void anv_loopback_set_state(anv_loopback_port_t *port, anv_link_state_t state, uint8_t tec, uint8_t rec)
{
    port->status.state = state;
    port->status.tec = tec;
    port->status.rec = rec;
}
