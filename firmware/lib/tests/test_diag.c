/* anv_node diagnostics: events (broadcast + rate limit), the black box surviving a reboot, the
 * `log` transfer that reads it back, and params_corrupt being reported automatically.
 */
#include <string.h>

#include "anv_motor_controller.h"
#include "minitest.h"
#include "node_harness.h"

#define NODE 11
#define CODE ANV_EVENT_CAN_ERROR_PASSIVE /* 0x0101, severity warning, a protocol-level code */

static anv_node_t *n;

static void boot(void)
{
    h_reset();
    h_config(0, &anv_motor_controller_type)->fixed_id = NODE;
    MT_CHECK(h_start(0));
    n = &h_nodes[0].node;
}

static bool fetch_log(void)
{
    const uint8_t req[2] = {2 /* LOG */, 0 /* REQUEST */};
    h_transfer_bind(NODE);
    h_transfer_last_rx.got = false;
    h_transfer_send(req, sizeof req);
    for (int t = 0; t < 200 && !h_transfer_last_rx.got; t++) {
        h_step(1);
    }
    return h_transfer_last_rx.got;
}

static void test_event_broadcast_and_rate_limit(void)
{
    boot();
    anv_node_event(n, CODE, 42);
    h_step(1);
    anv_event_t ev;
    const anv_link_frame_t *f = h_last(ANV_EVENT_PORT, NODE);
    MT_CHECK(f != NULL && anv_event_unpack(&ev, f->data, f->len));
    MT_CHECK(ev.code == CODE && ev.detail == 42 && ev.severity == ANV_SEVERITY_WARNING && !ev.cleared);
    MT_CHECK(anv_node_diag_log_count(n) == 1u);

    /* Same code again right away: event_rate_limit defaults to 1 Hz, so it's dropped (bus and
     * the black box both). */
    const int before = h_count(ANV_EVENT_PORT, NODE);
    anv_node_event(n, CODE, 99);
    h_step(1);
    MT_CHECK(h_count(ANV_EVENT_PORT, NODE) == before);
    MT_CHECK(anv_node_diag_log_count(n) == 1u);

    /* A second passes: it goes through again. */
    h_step(1000);
    anv_node_event(n, CODE, 7);
    h_step(1);
    MT_CHECK(h_count(ANV_EVENT_PORT, NODE) == before + 1);
    MT_CHECK(anv_node_diag_log_count(n) == 2u);
    const anv_diag_entry_t *e = anv_node_diag_log_entry(n, 1);
    MT_CHECK(e != NULL && e->code == CODE && e->detail == 7);

    /* A different code is never rate-limited against the first. */
    anv_node_event(n, ANV_EVENT_CAN_BUS_OFF, 1);
    h_step(1);
    MT_CHECK(anv_node_diag_log_count(n) == 3u);
}

static void test_clear_is_never_rate_limited(void)
{
    boot();
    anv_node_event(n, CODE, 1);
    anv_node_event_clear(n, CODE, 0);
    h_step(1);
    MT_CHECK(anv_node_diag_log_count(n) == 2u);
    const anv_diag_entry_t *e = anv_node_diag_log_entry(n, 1);
    MT_CHECK(e != NULL && e->cleared);
}

static void test_log_wraps_and_keeps_newest(void)
{
    boot();
    for (unsigned i = 0; i < ANV_NODE_DIAG_LOG_CAP + 5u; i++) {
        anv_node_event(n, (uint16_t)(0x4100u + i), (uint16_t)i); /* a fresh code each time: never limited */
    }
    MT_CHECK(anv_node_diag_log_count(n) == ANV_NODE_DIAG_LOG_CAP);
    const anv_diag_entry_t *oldest = anv_node_diag_log_entry(n, 0);
    const anv_diag_entry_t *newest = anv_node_diag_log_entry(n, ANV_NODE_DIAG_LOG_CAP - 1u);
    MT_CHECK(oldest != NULL && oldest->detail == 5u);                         /* the first 5 fell off */
    MT_CHECK(newest != NULL && newest->detail == ANV_NODE_DIAG_LOG_CAP + 4u);
}

static void test_survives_reboot(void)
{
    boot();
    anv_node_event(n, CODE, 1);
    h_step(1200);
    anv_node_event(n, ANV_EVENT_CAN_BUS_OFF, 2);
    MT_CHECK(anv_node_diag_log_count(n) == 2u);

    MT_CHECK(h_start(0)); /* "reboot": same memstore, fresh anv_node_t */
    n = &h_nodes[0].node;
    MT_CHECK(anv_node_diag_log_count(n) == 2u);
    const anv_diag_entry_t *e0 = anv_node_diag_log_entry(n, 0);
    const anv_diag_entry_t *e1 = anv_node_diag_log_entry(n, 1);
    MT_CHECK(e0 != NULL && e0->code == CODE && e0->seq == 1u);
    MT_CHECK(e1 != NULL && e1->code == ANV_EVENT_CAN_BUS_OFF && e1->seq == 2u);

    /* New events keep the sequence numbers going instead of restarting at 1. */
    anv_node_event(n, ANV_EVENT_CAN_RX_OVERFLOW, 3);
    h_step(1);
    const anv_diag_entry_t *e2 = anv_node_diag_log_entry(n, 2);
    MT_CHECK(e2 != NULL && e2->seq == 3u);
}

static void test_log_transfer(void)
{
    boot();
    anv_node_event(n, CODE, 11);
    h_step(1200);
    anv_node_event(n, ANV_EVENT_CAN_BUS_OFF, 22);
    h_step(1);

    MT_CHECK(fetch_log());
    const uint8_t *d = h_transfer_last_rx.data;
    MT_CHECK(h_transfer_last_rx.len == 2u + 2u * 8u); /* service + msg_type + 2 entries */
    MT_CHECK(d[0] == 2u /* LOG */ && d[1] == 1u /* DATA */);
    const uint16_t code0 = (uint16_t)(d[2] | (d[3] << 8));
    const uint16_t detail0 = (uint16_t)(d[4] | (d[5] << 8));
    MT_CHECK(code0 == CODE && detail0 == 11u);
    const uint16_t code1 = (uint16_t)(d[10] | (d[11] << 8));
    const uint16_t detail1 = (uint16_t)(d[12] | (d[13] << 8));
    MT_CHECK(code1 == ANV_EVENT_CAN_BUS_OFF && detail1 == 22u);
}

static void test_params_corrupt_reported(void)
{
    h_reset();
    anv_node_config_t *c = h_config(0, &anv_motor_controller_type);
    c->fixed_id = NODE;
    uint8_t garbage[40]; /* long enough to pass the "nothing saved yet" check and hit the CRC one */
    memset(garbage, 0xFF, sizeof garbage);
    MT_CHECK(anv_node_memstore_ops()->save(&h_nodes[0].store, "anv_params", garbage, sizeof garbage));
    MT_CHECK(h_start(0));
    n = &h_nodes[0].node;
    h_step(1);
    anv_event_t ev;
    const anv_link_frame_t *f = h_last(ANV_EVENT_PORT, NODE);
    MT_CHECK(f != NULL && anv_event_unpack(&ev, f->data, f->len) && ev.code == ANV_EVENT_PARAMS_CORRUPT);
    anv_value_t hz;
    MT_CHECK(anv_node_get(n, ANV_PARAM_HEARTBEAT_HZ, &hz) && hz.u == 10u); /* the default, not garbage */
}

int main(void)
{
    MT_RUN(test_event_broadcast_and_rate_limit);
    MT_RUN(test_clear_is_never_rate_limited);
    MT_RUN(test_log_wraps_and_keeps_newest);
    MT_RUN(test_survives_reboot);
    MT_RUN(test_log_transfer);
    MT_RUN(test_params_corrupt_reported);
    return mt_summary();
}
