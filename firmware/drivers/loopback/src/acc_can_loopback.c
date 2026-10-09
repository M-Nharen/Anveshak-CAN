#include "acc_can_loopback.h"

static bool filter_accepts(const acc_can_loopback_port_t *port, uint16_t id)
{
    if (port->n_filters == 0u) {
        return true;
    }
    for (size_t i = 0; i < port->n_filters; i++) {
        const acc_can_filter_t *f = &port->filters[i];
        if ((id & f->mask) == (f->id & f->mask)) {
            return true;
        }
    }
    return false;
}

static int loopback_init(void *ctx, uint32_t bitrate_bps, const acc_can_filter_t *filters, size_t n_filters)
{
    (void)bitrate_bps;
    acc_can_loopback_port_t *port = (acc_can_loopback_port_t *)ctx;

    if (n_filters > ACC_CAN_MAX_FILTERS) {
        return ACC_CAN_ERR_INVALID_ARG;
    }
    port->n_filters = n_filters;
    for (size_t i = 0; i < n_filters; i++) {
        port->filters[i] = filters[i];
    }

    port->error_state.tec = 0;
    port->error_state.rec = 0;
    port->error_state.state = ACC_CAN_ERROR_ACTIVE;
    port->error_state.bus_off_count = 0;

    anv_frame_ring_init(&port->rx_ring, port->rx_storage, ACC_CAN_LOOPBACK_RX_CAPACITY);
    return ACC_CAN_OK;
}

static int loopback_send(void *ctx, const acc_can_frame_t *frame, acc_can_priority_t prio)
{
    (void)prio;
    acc_can_loopback_port_t *sender = (acc_can_loopback_port_t *)ctx;

    if (sender->error_state.state == ACC_CAN_BUS_OFF) {
        return ACC_CAN_ERR_NOT_READY;
    }
    if (sender->bus == NULL) {
        return ACC_CAN_ERR_BACKEND;
    }

    for (size_t i = 0; i < sender->bus->count; i++) {
        acc_can_loopback_port_t *other = sender->bus->ports[i];
        if (other == sender) {
            continue;
        }
        if (filter_accepts(other, frame->id)) {
            (void)anv_frame_ring_push(&other->rx_ring, frame);
        }
    }
    return ACC_CAN_OK;
}

static bool loopback_receive(void *ctx, acc_can_frame_t *frame)
{
    acc_can_loopback_port_t *port = (acc_can_loopback_port_t *)ctx;
    return anv_frame_ring_pop(&port->rx_ring, frame);
}

static int loopback_get_error_state(void *ctx, acc_can_error_state_t *state)
{
    const acc_can_loopback_port_t *port = (const acc_can_loopback_port_t *)ctx;
    *state = port->error_state;
    return ACC_CAN_OK;
}

static int loopback_start_recovery(void *ctx)
{
    acc_can_loopback_port_t *port = (acc_can_loopback_port_t *)ctx;
    if (port->error_state.state == ACC_CAN_BUS_OFF) {
        port->error_state.state = ACC_CAN_ERROR_ACTIVE;
        port->error_state.tec = 0;
        port->error_state.rec = 0;
        port->error_state.bus_off_count++;
    }
    return ACC_CAN_OK;
}

void acc_can_loopback_bus_init(acc_can_loopback_bus_t *bus)
{
    bus->count = 0;
    for (size_t i = 0; i < ACC_CAN_LOOPBACK_MAX_PORTS; i++) {
        bus->ports[i] = NULL;
    }
}

bool acc_can_loopback_attach(acc_can_loopback_bus_t *bus, acc_can_loopback_port_t *port)
{
    if (bus->count >= ACC_CAN_LOOPBACK_MAX_PORTS) {
        return false;
    }
    port->bus = bus;
    port->n_filters = 0;
    anv_frame_ring_init(&port->rx_ring, port->rx_storage, ACC_CAN_LOOPBACK_RX_CAPACITY);
    bus->ports[bus->count] = port;
    bus->count++;
    return true;
}

acc_can_driver_t acc_can_loopback_make_driver(acc_can_loopback_port_t *port)
{
    acc_can_driver_t driver;
    driver.ctx = port;
    driver.init = loopback_init;
    driver.send = loopback_send;
    driver.receive = loopback_receive;
    driver.get_error_state = loopback_get_error_state;
    driver.start_recovery = loopback_start_recovery;
    return driver;
}

void acc_can_loopback_force_bus_off(acc_can_loopback_port_t *port)
{
    port->error_state.state = ACC_CAN_BUS_OFF;
}
