/* Test bench for anv_node: nodes and one fake host on the loopback bus, with a simulated clock.
 * The host side is written by hand from the generated pack/unpack code, the way a real host
 * would talk to a node. */
#ifndef NODE_HARNESS_H
#define NODE_HARNESS_H

#include "anv_link_loopback.h"
#include "anv_node.h"

#define H_MAX_NODES 3
#define H_LOG 512
#define H_HOST_ID 1 /* the fake host's node ID (hosts get IDs from config) */

/* What a node's application hooks saw. */
typedef struct {
    int outputs_off;
    int setups;
    int loops;
    int commands;
    uint16_t last_command_port;
    uint8_t last_command[8];
    int syncs;
    int identifies;
    int resets;
    int setting_changes;
    uint16_t last_setting;
    int actions;
    uint16_t last_action;
    uint32_t last_action_arg;
    int raw_frames;
    int streams_filled;
} h_app_t;

typedef struct {
    anv_loopback_port_t port;
    anv_link_t link;
    anv_node_t node;
    anv_node_memstore_t store;
    h_app_t app;
} h_node_t;

extern anv_loopback_bus_t h_bus;
extern h_node_t h_nodes[H_MAX_NODES];
extern uint32_t h_now;
/* Every frame the host received, in order. */
extern anv_link_frame_t h_log[H_LOG];
extern int h_log_n;

void h_reset(void);
/* Configure node i (storage = its memstore, mac = {2,0,0,0,0,i+1}); edit h_cfg before h_start. */
anv_node_config_t *h_config(int i, const anv_type_desc_t *type);
/* anv_node_init + anv_node_begin; keeps the memstore (a "reboot" when called again). */
bool h_start(int i);
/* Advance the clock by ms, 1 ms at a time, polling every started node and the host. */
void h_step(uint32_t ms);

void h_send(uint8_t priority, anv_node_id_t dst, uint16_t port, const uint8_t *data, uint8_t len);
void h_host_heartbeat(anv_node_id_t src, uint8_t role, uint16_t term);
void h_nmt(anv_node_id_t dst, uint8_t command, uint16_t term);
void h_estop(anv_node_id_t src);
/* Send a SERVICE request and wait for the answer; false if none came within 50 ms. */
bool h_service(anv_node_id_t dst, uint8_t op, uint16_t index, uint32_t value, anv_service_t *resp);
/* Keep a leader (H_HOST_ID, term) alive while stepping, as a real host would at 20 Hz. */
void h_step_with_leader(uint32_t ms, uint16_t term);

int h_count(uint16_t port, anv_node_id_t src);
const anv_link_frame_t *h_last(uint16_t port, anv_node_id_t src);
void h_clear_log(void);

#endif /* NODE_HARNESS_H */
