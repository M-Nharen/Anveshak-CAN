/* Shared between the anv_node source files; not part of the API. */
#ifndef ANV_NODE_INTERNAL_H
#define ANV_NODE_INTERNAL_H

#include "anv_node.h"

/* Storage key of the saved node ID (settings use "anv_params", see anv_node_settings.c). */
#define ANV_NODE_KEY_ID "anv_id"

bool anv_node_i_armed(const anv_node_t *n);
bool anv_node_i_send(anv_node_t *n, uint8_t priority, anv_node_id_t dst, uint16_t port, const uint8_t *data,
                     uint8_t len);
/* Re-derive n->state from the current latches (estop, fault, ota.phase, armed request). For
 * anv_node_diag.c / anv_node_ota.c, which change some of those latches themselves. */
void anv_node_i_refresh_state(anv_node_t *n);

uint32_t anv_node_i_param_u(const anv_node_t *n, uint16_t index);
void anv_node_i_load_defaults(anv_node_t *n);
void anv_node_i_load_saved(anv_node_t *n);
/* from_host: refuse read-only parameters (the runtime itself may set them) */
uint8_t anv_node_i_set_param(anv_node_t *n, uint16_t index, anv_value_t v, bool from_host);
void anv_node_i_on_service(anv_node_t *n, anv_node_id_t src, const anv_link_frame_t *f);

/* anv_node_diag.c */
void anv_node_i_diag_load(anv_node_t *n); /* from anv_node_init(): load the saved black box */
uint16_t anv_node_i_diag_build_log(const anv_node_t *n, uint8_t *out, uint16_t cap);
/* The `log` transfer_service, dispatched the same way as anv_node_i_ota_handle. */
void anv_node_i_diag_handle(anv_node_t *n, anv_node_id_t src, const uint8_t *d, uint16_t len);

/* anv_node_ota.c: all dispatched from the `transfer` port once anv_node.c has stripped the
 * leading transfer_service byte and reassembled the ISO-TP payload (d[0] is this service's own
 * message type). */
void anv_node_i_ota_handle(anv_node_t *n, anv_node_id_t src, const uint8_t *d, uint16_t len);
void anv_node_i_ota_poll(anv_node_t *n); /* every anv_node_poll(): session timeout, confirm timer */
void anv_node_i_ota_boot_check(anv_node_t *n); /* from anv_node_begin(): rollback decision */

#endif /* ANV_NODE_INTERNAL_H */
