/* anv_proto: the v2 protocol's 29-bit identifier, on top of anv_link.
 *
 * The field widths and positions come from the generated anv_protocol.h (schema/protocol.yaml
 * `id_layout`); nothing here hard-codes them. ISO-TP for transfers over 8 bytes is in
 * anv_isotp.h.
 */
#ifndef ANV_PROTO_H
#define ANV_PROTO_H

#include "anv_link.h"
#include "anv_proto_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The layout must fill a 29-bit identifier exactly, from the top down. */
#define ANV_PROTO_ID_BITS (ANV_ID_PRIORITY_BITS + ANV_ID_DST_BITS + ANV_ID_PORT_BITS + ANV_ID_SRC_BITS)
#if defined(__cplusplus)
static_assert(ANV_PROTO_ID_BITS == 29u, "protocol.yaml id_layout must total 29 bits");
#else
_Static_assert(ANV_PROTO_ID_BITS == 29u, "protocol.yaml id_layout must total 29 bits");
#endif

#define ANV_PROTO_FIELD_MASK(field) (((1u << ANV_ID_##field##_BITS) - 1u) << ANV_ID_##field##_SHIFT)
#define ANV_PROTO_PRIORITY_MASK ANV_PROTO_FIELD_MASK(PRIORITY)
#define ANV_PROTO_DST_MASK ANV_PROTO_FIELD_MASK(DST)
#define ANV_PROTO_PORT_MASK ANV_PROTO_FIELD_MASK(PORT)
#define ANV_PROTO_SRC_MASK ANV_PROTO_FIELD_MASK(SRC)

/* An identifier, unpacked. */
typedef struct {
    uint8_t priority;  /* 0 = most urgent */
    anv_node_id_t dst; /* ANV_NODE_BROADCAST = everyone */
    uint16_t port;
    anv_node_id_t src;
} anv_proto_addr_t;

static inline uint32_t anv_proto_id(const anv_proto_addr_t *a)
{
    return anv_id_make(a->priority, a->dst, a->port, a->src);
}

static inline anv_proto_addr_t anv_proto_addr(uint32_t id)
{
    anv_proto_addr_t a;
    a.priority = anv_id_priority(id);
    a.dst = anv_id_dst(id);
    a.port = anv_id_port(id);
    a.src = anv_id_src(id);
    return a;
}

/* False if a field doesn't fit its width (anv_id_make() would silently truncate it). */
bool anv_proto_addr_valid(const anv_proto_addr_t *a);

/* Fill a frame; false (frame untouched) if the address is invalid or len > 8. */
bool anv_proto_frame(anv_link_frame_t *frame, const anv_proto_addr_t *a, const void *data, uint8_t len);

/* Is a frame with this identifier for node `self`: addressed to it, or broadcast? */
static inline bool anv_proto_for_node(uint32_t id, anv_node_id_t self)
{
    const anv_node_id_t dst = anv_id_dst(id);
    return dst == self || dst == ANV_NODE_BROADCAST;
}

/* Filters that accept exactly the frames addressed to `self` or broadcast. Both test only the
 * dst field, which protocol.yaml keeps in the top 16 bits so the ESP32 can do this in hardware.
 * Writes two filters; returns 2. */
uint8_t anv_proto_node_filters(anv_node_id_t self, anv_link_filter_t out[2]);

/* A filter for one port from any source to any destination. */
anv_link_filter_t anv_proto_port_filter(uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* ANV_PROTO_H */
