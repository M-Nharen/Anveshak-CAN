/* anv_link front end over the loopback bus: delivery, filters, counters, bus-off recovery. */
#include <string.h>

#include "anv_link_loopback.h"
#include "anv_proto.h"
#include "minitest.h"

static anv_loopback_bus_t bus;
static anv_loopback_port_t port_a, port_b, port_c;
static anv_link_t a, b, c;

static void setup(void)
{
    anv_loopback_bus_init(&bus);
    anv_loopback_attach(&bus, &port_a);
    anv_loopback_attach(&bus, &port_b);
    anv_loopback_attach(&bus, &port_c);
    anv_link_init(&a, anv_loopback_ops(), &port_a);
    anv_link_init(&b, anv_loopback_ops(), &port_b);
    anv_link_init(&c, anv_loopback_ops(), &port_c);
    anv_link_config_t cfg;
    anv_link_config_default(&cfg);
    MT_CHECK(anv_link_start(&a, &cfg) == ANV_LINK_OK);
    MT_CHECK(anv_link_start(&b, &cfg) == ANV_LINK_OK);
    MT_CHECK(anv_link_start(&c, &cfg) == ANV_LINK_OK);
}

static anv_link_frame_t frame_to(anv_node_id_t dst, uint16_t port, uint8_t marker)
{
    anv_link_frame_t f;
    const anv_proto_addr_t addr = {4, dst, port, 1};
    anv_proto_frame(&f, &addr, &marker, 1);
    return f;
}

static int drain(anv_link_t *l)
{
    anv_link_frame_t f;
    int n = 0;
    while (anv_link_receive(l, &f)) {
        n++;
    }
    return n;
}

static void test_defaults(void)
{
    anv_link_config_t cfg;
    anv_link_config_default(&cfg);
    MT_CHECK(cfg.bitrate == 500000u);
    MT_CHECK(cfg.n_filters == 0u);
    MT_CHECK(cfg.auto_recover);
}

static void test_send_receive_in_order(void)
{
    setup();
    for (uint8_t i = 0; i < 20; i++) {
        anv_link_frame_t f = frame_to(5, 100, i);
        MT_CHECK(anv_link_send(&a, &f) == ANV_LINK_OK);
    }
    anv_link_frame_t got;
    for (uint8_t i = 0; i < 20; i++) {
        MT_CHECK(anv_link_receive(&b, &got));
        MT_CHECK(got.data[0] == i && got.len == 1);
        MT_CHECK(anv_id_dst(got.id) == 5 && anv_id_port(got.id) == 100);
    }
    MT_CHECK(!anv_link_receive(&b, &got));
    MT_CHECK(drain(&a) == 0); /* a sender doesn't hear itself */
    MT_CHECK(drain(&c) == 20);
    MT_CHECK(anv_link_stats(&a)->tx_frames == 20);
    MT_CHECK(anv_link_stats(&b)->rx_frames == 20);
}

static void test_bad_frames_refused(void)
{
    setup();
    anv_link_frame_t f = frame_to(5, 100, 0);
    f.len = 9;
    MT_CHECK(anv_link_send(&a, &f) == ANV_LINK_ERR_ARG);
    f.len = 1;
    f.id = 0x20000000u;
    MT_CHECK(anv_link_send(&a, &f) == ANV_LINK_ERR_ARG);
    anv_link_t idle;
    anv_link_init(&idle, anv_loopback_ops(), &port_a);
    f.id = 1;
    MT_CHECK(anv_link_send(&idle, &f) == ANV_LINK_ERR_NOT_READY);
    anv_link_config_t cfg;
    anv_link_config_default(&cfg);
    cfg.n_filters = ANV_LINK_MAX_FILTERS + 1u;
    MT_CHECK(anv_link_start(&idle, &cfg) == ANV_LINK_ERR_ARG);
}

static void test_filters_at_start_and_run_time(void)
{
    setup();
    anv_link_config_t cfg;
    anv_link_config_default(&cfg);
    cfg.n_filters = anv_proto_node_filters(7, cfg.filters);
    anv_link_stop(&b);
    MT_CHECK(anv_link_start(&b, &cfg) == ANV_LINK_OK);

    anv_link_frame_t to7 = frame_to(7, 100, 1), to8 = frame_to(8, 100, 2), all = frame_to(0, 100, 3);
    anv_link_send(&a, &to7);
    anv_link_send(&a, &to8);
    anv_link_send(&a, &all);
    anv_link_frame_t got;
    MT_CHECK(anv_link_receive(&b, &got) && got.data[0] == 1);
    MT_CHECK(anv_link_receive(&b, &got) && got.data[0] == 3);
    MT_CHECK(!anv_link_receive(&b, &got));
    MT_CHECK(anv_link_stats(&b)->rx_filtered == 1);

    /* The node gets a new ID: only frames to 8 (and broadcast) now. */
    anv_link_filter_t flt[2];
    MT_CHECK(anv_link_set_filters(&b, flt, anv_proto_node_filters(8, flt)) == ANV_LINK_OK);
    anv_link_send(&a, &to7);
    anv_link_send(&a, &to8);
    MT_CHECK(anv_link_receive(&b, &got) && got.data[0] == 2);
    MT_CHECK(!anv_link_receive(&b, &got));

    /* Port filter plus "everything" on another link. */
    const anv_link_filter_t pf = anv_proto_port_filter(ANV_TRANSFER_PORT);
    MT_CHECK(anv_link_set_filters(&c, &pf, 1) == ANV_LINK_OK);
    drain(&c);
    anv_link_frame_t tp = frame_to(9, ANV_TRANSFER_PORT, 4);
    anv_link_send(&a, &to7);
    anv_link_send(&a, &tp);
    MT_CHECK(anv_link_receive(&c, &got) && got.data[0] == 4);
    MT_CHECK(!anv_link_receive(&c, &got));
    MT_CHECK(anv_link_set_filters(&c, NULL, 0) == ANV_LINK_OK);
    anv_link_send(&a, &to7);
    MT_CHECK(drain(&c) == 1);
    MT_CHECK(anv_link_set_filters(&c, NULL, 1) == ANV_LINK_ERR_ARG);
}

static void test_rx_overflow_counted(void)
{
    setup();
    port_b.rx_capacity = 4;
    for (uint8_t i = 0; i < 10; i++) {
        anv_link_frame_t f = frame_to(5, 100, i);
        anv_link_send(&a, &f);
    }
    MT_CHECK(anv_link_poll(&b, 0) & ANV_LINK_EV_RX_OVERFLOW);
    MT_CHECK(anv_link_stats(&b)->rx_overflow == 6);
    MT_CHECK(drain(&b) == 4);
    MT_CHECK((anv_link_poll(&b, 1) & ANV_LINK_EV_RX_OVERFLOW) == 0u);
}

static void test_passive_and_bus_off_recovery(void)
{
    setup();
    anv_loopback_set_state(&port_a, ANV_LINK_STATE_PASSIVE, 130, 5);
    uint32_t ev = anv_link_poll(&a, 0);
    MT_CHECK(ev & ANV_LINK_EV_PASSIVE);
    MT_CHECK(anv_link_stats(&a)->tec == 130 && anv_link_stats(&a)->rec == 5);
    MT_CHECK(anv_link_poll(&a, 1) == 0u); /* reported once */

    anv_loopback_set_state(&port_a, ANV_LINK_STATE_BUS_OFF, 255, 0);
    ev = anv_link_poll(&a, 10);
    MT_CHECK(ev & ANV_LINK_EV_BUS_OFF);
    MT_CHECK(anv_link_stats(&a)->bus_off_count == 1);
    MT_CHECK(port_a.recover_requests == 1);
    /* The loopback recovers at once; the next poll sees it. */
    ev = anv_link_poll(&a, 20);
    MT_CHECK(ev & ANV_LINK_EV_RECOVERED);
    MT_CHECK(anv_link_stats(&a)->recoveries == 1);
    MT_CHECK(anv_link_stats(&a)->state == ANV_LINK_STATE_ACTIVE);
    anv_link_frame_t f = frame_to(5, 100, 1);
    MT_CHECK(anv_link_send(&a, &f) == ANV_LINK_OK);
}

static void test_bus_off_retry_and_send_refused(void)
{
    setup();
    port_a.hold_bus_off = true;
    anv_loopback_set_state(&port_a, ANV_LINK_STATE_BUS_OFF, 255, 0);
    anv_link_poll(&a, 1000);
    MT_CHECK(port_a.recover_requests == 1);
    anv_link_frame_t f = frame_to(5, 100, 1);
    MT_CHECK(anv_link_send(&a, &f) == ANV_LINK_ERR_NOT_READY);
    MT_CHECK(anv_link_stats(&a)->tx_rejected == 1);
    MT_CHECK(drain(&b) == 0);

    anv_link_poll(&a, 1500); /* before recover_retry_ms: no new request */
    MT_CHECK(port_a.recover_requests == 1);
    anv_link_poll(&a, 2000);
    MT_CHECK(port_a.recover_requests == 2);
    MT_CHECK(anv_link_stats(&a)->bus_off_count == 1); /* still the same bus-off */

    port_a.hold_bus_off = false;
    anv_link_poll(&a, 3000);
    MT_CHECK(anv_link_poll(&a, 3001) & ANV_LINK_EV_RECOVERED);
    MT_CHECK(anv_link_send(&a, &f) == ANV_LINK_OK);
}

static void test_manual_recovery(void)
{
    setup();
    anv_link_config_t cfg;
    anv_link_config_default(&cfg);
    cfg.auto_recover = false;
    anv_link_stop(&a);
    anv_link_start(&a, &cfg);
    anv_loopback_set_state(&port_a, ANV_LINK_STATE_BUS_OFF, 255, 0);
    MT_CHECK(anv_link_poll(&a, 0) & ANV_LINK_EV_BUS_OFF);
    anv_link_poll(&a, 5000);
    MT_CHECK(port_a.recover_requests == 0);
    MT_CHECK(anv_link_recover(&a) == ANV_LINK_OK);
    MT_CHECK(anv_link_poll(&a, 5001) & ANV_LINK_EV_RECOVERED);
}

int main(void)
{
    MT_RUN(test_defaults);
    MT_RUN(test_send_receive_in_order);
    MT_RUN(test_bad_frames_refused);
    MT_RUN(test_filters_at_start_and_run_time);
    MT_RUN(test_rx_overflow_counted);
    MT_RUN(test_passive_and_bus_off_recovery);
    MT_RUN(test_bus_off_retry_and_send_refused);
    MT_RUN(test_manual_recovery);
    return mt_summary();
}
