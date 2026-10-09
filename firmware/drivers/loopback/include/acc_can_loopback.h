/* In-process loopback CAN driver: several "nodes" (ports) attached to one shared virtual
 * bus, for host-side unit tests with no hardware and no vcan0. Implements the acc_can_driver_t
 * interface used by anveshak_can_core. */
#ifndef ACC_CAN_LOOPBACK_H
#define ACC_CAN_LOOPBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acc_can.h"
#include "anv_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACC_CAN_LOOPBACK_MAX_PORTS 8u
#define ACC_CAN_LOOPBACK_RX_CAPACITY 64u

typedef struct acc_can_loopback_port_s acc_can_loopback_port_t;

typedef struct {
    acc_can_loopback_port_t *ports[ACC_CAN_LOOPBACK_MAX_PORTS];
    size_t count;
} acc_can_loopback_bus_t;

struct acc_can_loopback_port_s {
    acc_can_loopback_bus_t *bus;
    acc_can_filter_t filters[ACC_CAN_MAX_FILTERS];
    size_t n_filters;
    anv_frame_ring_t rx_ring;
    acc_can_frame_t rx_storage[ACC_CAN_LOOPBACK_RX_CAPACITY];
    acc_can_error_state_t error_state;
};

void acc_can_loopback_bus_init(acc_can_loopback_bus_t *bus);

/* Attach `port` to `bus` before building its driver vtable. Returns false if the bus is full. */
bool acc_can_loopback_attach(acc_can_loopback_bus_t *bus, acc_can_loopback_port_t *port);

/* Builds the acc_can_driver_t vtable for an attached port. */
acc_can_driver_t acc_can_loopback_make_driver(acc_can_loopback_port_t *port);

/* Test-only hooks to simulate bus errors directly, without needing real electrical noise. */
void acc_can_loopback_force_bus_off(acc_can_loopback_port_t *port);

#ifdef __cplusplus
}
#endif

#endif /* ACC_CAN_LOOPBACK_H */
