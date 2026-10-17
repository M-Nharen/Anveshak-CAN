/* In-process loopback bus for anv_link: several ports on one virtual bus, for Linux unit tests
 * (no hardware, no vcan). A frame sent by one port reaches every other started port, in order.
 * Test hooks: a tap that sees (and may drop or edit) every frame on the bus, forced error
 * states, and a small receive queue so overflow can be provoked. */
#ifndef ANV_LINK_LOOPBACK_H
#define ANV_LINK_LOOPBACK_H

#include "anv_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_LOOPBACK_MAX_PORTS 8u
#define ANV_LOOPBACK_RX_CAPACITY 64u

typedef struct anv_loopback_port_s anv_loopback_port_t;

/* Return false to drop the frame (it reaches nobody). The tap may change *frame. */
typedef bool (*anv_loopback_tap_t)(void *user, const anv_loopback_port_t *from, anv_link_frame_t *frame);

typedef struct {
    anv_loopback_port_t *ports[ANV_LOOPBACK_MAX_PORTS];
    uint8_t count;
    anv_loopback_tap_t tap;
    void *tap_user;
} anv_loopback_bus_t;

struct anv_loopback_port_s {
    anv_loopback_bus_t *bus;
    bool started;
    anv_link_frame_t rx[ANV_LOOPBACK_RX_CAPACITY];
    uint16_t rx_head;
    uint16_t rx_count;
    uint16_t rx_capacity; /* <= ANV_LOOPBACK_RX_CAPACITY; tests may lower it */
    anv_link_hw_status_t status;
    uint32_t recover_requests;
    bool hold_bus_off; /* test hook: recovery requests are counted but don't succeed */
};

void anv_loopback_bus_init(anv_loopback_bus_t *bus);
void anv_loopback_set_tap(anv_loopback_bus_t *bus, anv_loopback_tap_t tap, void *user);

/* Attach a port, then anv_link_init(link, anv_loopback_ops(), port). False if the bus is full. */
bool anv_loopback_attach(anv_loopback_bus_t *bus, anv_loopback_port_t *port);
const anv_link_ops_t *anv_loopback_ops(void);

/* Put a frame on the bus as if some other device sent it (bypasses the tap). */
void anv_loopback_inject(anv_loopback_bus_t *bus, const anv_link_frame_t *frame);

/* Test hooks: error states as a real controller would report them. */
void anv_loopback_set_state(anv_loopback_port_t *port, anv_link_state_t state, uint8_t tec, uint8_t rec);

#ifdef __cplusplus
}
#endif

#endif /* ANV_LINK_LOOPBACK_H */
