#include "anv_proto.h"

#include <string.h>

static bool fits(uint32_t v, uint32_t bits)
{
    return (v >> bits) == 0u;
}

bool anv_proto_addr_valid(const anv_proto_addr_t *a)
{
    return fits(a->priority, ANV_ID_PRIORITY_BITS) && fits(a->dst, ANV_ID_DST_BITS) &&
           fits(a->port, ANV_ID_PORT_BITS) && fits(a->src, ANV_ID_SRC_BITS);
}

bool anv_proto_frame(anv_link_frame_t *frame, const anv_proto_addr_t *a, const void *data, uint8_t len)
{
    if (!anv_proto_addr_valid(a) || len > ANV_LINK_MAX_DLC || (len > 0u && data == NULL)) {
        return false;
    }
    frame->id = anv_proto_id(a);
    frame->len = len;
    memset(frame->data, 0, sizeof(frame->data));
    if (len > 0u) {
        memcpy(frame->data, data, len);
    }
    return true;
}

uint8_t anv_proto_node_filters(anv_node_id_t self, anv_link_filter_t out[2])
{
    out[0].id = (uint32_t)self << ANV_ID_DST_SHIFT;
    out[0].mask = ANV_PROTO_DST_MASK;
    out[1].id = (uint32_t)ANV_NODE_BROADCAST << ANV_ID_DST_SHIFT;
    out[1].mask = ANV_PROTO_DST_MASK;
    return 2;
}

anv_link_filter_t anv_proto_port_filter(uint16_t port)
{
    anv_link_filter_t f;
    f.id = ((uint32_t)port << ANV_ID_PORT_SHIFT) & ANV_PROTO_PORT_MASK;
    f.mask = ANV_PROTO_PORT_MASK;
    return f;
}
