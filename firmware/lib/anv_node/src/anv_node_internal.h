/* Shared between the anv_node source files; not part of the API. */
#ifndef ANV_NODE_INTERNAL_H
#define ANV_NODE_INTERNAL_H

#include "anv_node.h"

/* Storage key of the saved node ID (settings use "anv_params", see anv_node_settings.c). */
#define ANV_NODE_KEY_ID "anv_id"

bool anv_node_i_armed(const anv_node_t *n);
bool anv_node_i_send(anv_node_t *n, uint8_t priority, anv_node_id_t dst, uint16_t port, const uint8_t *data,
                     uint8_t len);

uint32_t anv_node_i_param_u(const anv_node_t *n, uint16_t index);
void anv_node_i_load_defaults(anv_node_t *n);
void anv_node_i_load_saved(anv_node_t *n);
/* from_host: refuse read-only parameters (the runtime itself may set them) */
uint8_t anv_node_i_set_param(anv_node_t *n, uint16_t index, anv_value_t v, bool from_host);
void anv_node_i_on_service(anv_node_t *n, anv_node_id_t src, const anv_link_frame_t *f);

#endif /* ANV_NODE_INTERNAL_H */
