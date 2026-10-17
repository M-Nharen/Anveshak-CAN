/* Test bench for anv_node: nodes and one fake host on the loopback bus, with a simulated clock.
 * The host side is written by hand from the generated pack/unpack code, the way a real host
 * would talk to a node. */
#ifndef NODE_HARNESS_H
#define NODE_HARNESS_H

#include "anv_isotp.h"
#include "anv_link_loopback.h"
#include "anv_node.h"

#define H_MAX_NODES 3
#define H_LOG 512
#define H_HOST_ID 1 /* the fake host's node ID (hosts get IDs from config) */
#define H_TRANSFER_RX_CAP 4096

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

/* A fake OTA backend: no real flash, just two "slots" (0 and 1) tracked as a version number,
 * so tests can check a transfer landed, a bad one didn't, and a boot-time rollback works. */
typedef struct {
    uint32_t capacity;  /* begin() fails above this many bytes; 0 = no limit (set before h_start) */
    bool fail_writes;   /* test hook: write() always fails */
    bool active;        /* a begin() happened and hasn't committed or aborted yet */
    uint32_t written;
    uint8_t active_version; /* flips on commit(); flips back on rollback_and_reboot() */
    int commits, aborts, confirms, rollbacks;
} h_ota_t;

const anv_node_ota_backend_t *h_ota_ops(void);

typedef struct {
    anv_loopback_port_t port;
    anv_link_t link;
    anv_node_t node;
    anv_node_memstore_t store;
    h_app_t app;
    h_ota_t ota;
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

/* ISO-TP from the fake host (H_HOST_ID), for OTA and the log transfer. One channel: bind it to
 * whichever node you're about to talk to. */
typedef struct {
    anv_node_id_t peer;
    uint16_t len;
    uint8_t data[H_TRANSFER_RX_CAP];
    bool got;
} h_transfer_rx_t;
extern h_transfer_rx_t h_transfer_last_rx; /* the latest full transfer received from a node */

void h_transfer_bind(anv_node_id_t peer);
/* Drives the send over repeated h_step(1) calls (up to ~2 s); the result anv_isotp_send() would
 * have reported through on_tx_done. */
anv_isotp_result_t h_transfer_send(const uint8_t *data, uint16_t len);

#endif /* NODE_HARNESS_H */
