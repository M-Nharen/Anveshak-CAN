/* 29-bit identifier: round trips through the generated layout, filters for "me or broadcast". */
#include "anv_link_twai_filter.h"
#include "anv_proto.h"
#include "minitest.h"

static uint32_t rng_state = 12345u;
static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 1;
}

static void test_round_trip_every_field(void)
{
    /* Each field at its extremes, others random: nothing bleeds into a neighbour. */
    const uint32_t pmax = (1u << ANV_ID_PRIORITY_BITS) - 1u;
    const uint32_t dmax = (1u << ANV_ID_DST_BITS) - 1u;
    const uint32_t portmax = (1u << ANV_ID_PORT_BITS) - 1u;
    const uint32_t smax = (1u << ANV_ID_SRC_BITS) - 1u;
    for (int i = 0; i < 2000; i++) {
        anv_proto_addr_t a;
        a.priority = (uint8_t)(i % 7 == 0 ? pmax : rng() % (pmax + 1u));
        a.dst = (anv_node_id_t)(i % 5 == 0 ? dmax : rng() % (dmax + 1u));
        a.port = (uint16_t)(i % 3 == 0 ? portmax : rng() % (portmax + 1u));
        a.src = (anv_node_id_t)(i % 11 == 0 ? smax : rng() % (smax + 1u));
        MT_CHECK(anv_proto_addr_valid(&a));
        const uint32_t id = anv_proto_id(&a);
        MT_CHECK(id <= ANV_LINK_EXT_ID_MASK);
        const anv_proto_addr_t b = anv_proto_addr(id);
        MT_CHECK(b.priority == a.priority && b.dst == a.dst && b.port == a.port && b.src == a.src);
        MT_CHECK(anv_proto_id(&b) == id);
    }
}

static void test_layout_from_schema(void)
{
    /* Masks tile the 29 bits exactly once, priority on top (arbitration: lower wins). */
    MT_CHECK((ANV_PROTO_PRIORITY_MASK | ANV_PROTO_DST_MASK | ANV_PROTO_PORT_MASK | ANV_PROTO_SRC_MASK) ==
             ANV_LINK_EXT_ID_MASK);
    MT_CHECK((ANV_PROTO_PRIORITY_MASK & ANV_PROTO_DST_MASK) == 0u);
    MT_CHECK((ANV_PROTO_DST_MASK & ANV_PROTO_PORT_MASK) == 0u);
    MT_CHECK((ANV_PROTO_PORT_MASK & ANV_PROTO_SRC_MASK) == 0u);
    const anv_proto_addr_t urgent = {0, 200, 1000, 200};
    const anv_proto_addr_t lazy = {1, 0, 0, 0};
    MT_CHECK(anv_proto_id(&urgent) < anv_proto_id(&lazy));
    /* The protocol's own messages use the generated ports. */
    const anv_proto_addr_t estop = {ANV_ESTOP_PRIORITY, ANV_NODE_BROADCAST, ANV_ESTOP_PORT, 7};
    MT_CHECK(anv_id_port(anv_proto_id(&estop)) == ANV_ESTOP_PORT);
}

static void test_invalid_fields_refused(void)
{
    anv_link_frame_t f = {0};
    anv_proto_addr_t a = {0, 1, (uint16_t)(ANV_MAX_PORT + 1u), 2};
    MT_CHECK(!anv_proto_addr_valid(&a));
    MT_CHECK(!anv_proto_frame(&f, &a, "x", 1));
    a.port = 5;
    a.priority = (uint8_t)(1u << ANV_ID_PRIORITY_BITS);
    MT_CHECK(!anv_proto_frame(&f, &a, "x", 1));
    a.priority = 3;
    MT_CHECK(!anv_proto_frame(&f, &a, "123456789", 9));
    MT_CHECK(anv_proto_frame(&f, &a, "1234", 4));
    MT_CHECK(f.len == 4 && f.data[3] == '4' && f.data[4] == 0);
    MT_CHECK(anv_id_priority(f.id) == 3 && anv_id_port(f.id) == 5);
}

static void test_node_filters(void)
{
    const anv_node_id_t self = 42;
    anv_link_filter_t flt[2];
    MT_CHECK(anv_proto_node_filters(self, flt) == 2);
    for (int i = 0; i < 5000; i++) {
        const uint32_t id = rng() & ANV_LINK_EXT_ID_MASK;
        const bool want = anv_proto_for_node(id, self);
        MT_CHECK(anv_link_filters_accept(flt, 2, id) == want);
        /* The ESP32's dual hardware filter, programmed from these, is exact too: dst is in the
         * 16 bits it compares. */
        const anv_twai_hw_filter_t hw = anv_twai_hw_filter(flt, 2);
        MT_CHECK(hw.dual);
        MT_CHECK(anv_twai_hw_accepts(&hw, id) == want);
    }
    const anv_proto_addr_t mine = {4, self, 100, 9};
    const anv_proto_addr_t bcast = {4, ANV_NODE_BROADCAST, 100, 9};
    const anv_proto_addr_t other = {4, 43, 100, 9};
    MT_CHECK(anv_link_filters_accept(flt, 2, anv_proto_id(&mine)));
    MT_CHECK(anv_link_filters_accept(flt, 2, anv_proto_id(&bcast)));
    MT_CHECK(!anv_link_filters_accept(flt, 2, anv_proto_id(&other)));
}

static void test_port_filter(void)
{
    const anv_link_filter_t f = anv_proto_port_filter(ANV_TRANSFER_PORT);
    const anv_proto_addr_t yes = {6, 3, ANV_TRANSFER_PORT, 9};
    const anv_proto_addr_t no = {6, 3, ANV_TRANSFER_PORT + 1u, 9};
    MT_CHECK(anv_link_filters_accept(&f, 1, anv_proto_id(&yes)));
    MT_CHECK(!anv_link_filters_accept(&f, 1, anv_proto_id(&no)));
}

static void test_hw_filter_is_superset(void)
{
    /* Whatever the filters, the hardware must never reject a frame the exact filters accept. */
    for (uint8_t n = 1; n <= ANV_LINK_MAX_FILTERS; n++) {
        for (int round = 0; round < 50; round++) {
            anv_link_filter_t flt[ANV_LINK_MAX_FILTERS];
            for (uint8_t k = 0; k < n; k++) {
                flt[k].id = rng() & ANV_LINK_EXT_ID_MASK;
                flt[k].mask = rng() & rng() & ANV_LINK_EXT_ID_MASK;
            }
            const anv_twai_hw_filter_t hw = anv_twai_hw_filter(flt, n);
            for (int i = 0; i < 400; i++) {
                /* Half the IDs are built to match a filter, half are random. */
                uint32_t id = rng() & ANV_LINK_EXT_ID_MASK;
                if (i & 1) {
                    const anv_link_filter_t *f = &flt[(uint32_t)i % n];
                    id = (f->id & f->mask) | (id & ~f->mask);
                }
                if (anv_link_filters_accept(flt, n, id)) {
                    MT_CHECK(anv_twai_hw_accepts(&hw, id));
                }
            }
        }
    }
    /* One filter is programmed exactly. */
    const anv_link_filter_t one = {0x1234567u, 0x1FFFFF00u};
    const anv_twai_hw_filter_t hw = anv_twai_hw_filter(&one, 1);
    MT_CHECK(!hw.dual && hw.enabled);
    MT_CHECK(anv_twai_hw_accepts(&hw, 0x12345AAu));
    MT_CHECK(!anv_twai_hw_accepts(&hw, 0x12355AAu));
    const anv_twai_hw_filter_t none = anv_twai_hw_filter(NULL, 0);
    MT_CHECK(!none.enabled && anv_twai_hw_accepts(&none, 0x1FFFFFFFu));
}

int main(void)
{
    MT_RUN(test_round_trip_every_field);
    MT_RUN(test_layout_from_schema);
    MT_RUN(test_invalid_fields_refused);
    MT_RUN(test_node_filters);
    MT_RUN(test_port_filter);
    MT_RUN(test_hw_filter_is_superset);
    return mt_summary();
}
