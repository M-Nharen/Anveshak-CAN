/* ISO-TP between two nodes on the loopback bus: long transfers, block sizes, STmin, and what
 * happens when frames are lost, repeated, reordered or refused. */
#include <string.h>

#include "anv_isotp.h"
#include "anv_link_loopback.h"
#include "minitest.h"

#define ID_A 10u
#define ID_B 20u

typedef struct {
    anv_loopback_port_t port;
    anv_link_t link;
    anv_isotp_t tp;
    uint8_t rx_buf[ANV_ISOTP_MAX_LEN];
    uint8_t got[ANV_ISOTP_MAX_LEN];
    uint16_t got_len;
    int rx_count;
    int tx_done;
    anv_isotp_result_t tx_result;
    anv_isotp_result_t rx_error;
    int rx_errors;
    bool refuse; /* the link "is full" */
} node_t;

static anv_loopback_bus_t bus;
static anv_loopback_port_t other_port;
static anv_link_t other_link;
static node_t na, nb;
static uint32_t now_us;

/* --- tap: what happens on the wire ------------------------------------------------------ */
typedef enum { WIRE_OK, WIRE_DROP_CF, WIRE_SWAP_CF, WIRE_DUP_CF, WIRE_DROP_FC, WIRE_DROP_LAST } wire_mode_t;
static wire_mode_t wire;
static int wire_cf_index; /* which consecutive frame (1-based, counted per transfer) */
static int cf_seen;
static anv_link_frame_t held;
static bool holding;
static uint32_t cf_times[600];
static int n_cf_times;

static bool is_pci(const anv_link_frame_t *f, uint8_t pci)
{
    return anv_id_port(f->id) == ANV_TRANSFER_PORT && (f->data[0] >> 4) == pci;
}

static bool tap(void *user, const anv_loopback_port_t *from, anv_link_frame_t *f)
{
    (void)user;
    (void)from;
    if (is_pci(f, 1)) {
        cf_seen = 0;
    }
    if (is_pci(f, 3) && wire == WIRE_DROP_FC) {
        return false;
    }
    if (!is_pci(f, 2)) {
        return true;
    }
    cf_seen++;
    if (n_cf_times < (int)(sizeof cf_times / sizeof cf_times[0])) {
        cf_times[n_cf_times++] = now_us;
    }
    if (cf_seen == wire_cf_index) {
        switch (wire) {
        case WIRE_DROP_CF:
            return false;
        case WIRE_SWAP_CF:
            held = *f;
            holding = true;
            return false;
        case WIRE_DUP_CF:
            anv_loopback_inject(&bus, f);
            return true;
        default:
            break;
        }
    }
    if (wire == WIRE_SWAP_CF && holding && cf_seen == wire_cf_index + 1) {
        anv_loopback_inject(&bus, f);
        anv_loopback_inject(&bus, &held);
        holding = false;
        return false;
    }
    return true;
}

/* --- nodes ------------------------------------------------------------------------------ */
static bool link_send(void *user, const anv_link_frame_t *f)
{
    node_t *n = (node_t *)user;
    return !n->refuse && anv_link_send(&n->link, f) == ANV_LINK_OK;
}

static void on_rx(void *user, const uint8_t *data, uint16_t len)
{
    node_t *n = (node_t *)user;
    memcpy(n->got, data, len);
    n->got_len = len;
    n->rx_count++;
}

static void on_tx_done(void *user, anv_isotp_result_t r)
{
    node_t *n = (node_t *)user;
    n->tx_done++;
    n->tx_result = r;
}

static void on_rx_error(void *user, anv_isotp_result_t r)
{
    node_t *n = (node_t *)user;
    n->rx_errors++;
    n->rx_error = r;
}

static void node_setup(node_t *n, anv_node_id_t self, anv_node_id_t peer, uint8_t bs, uint8_t st_min,
                       uint16_t rx_size)
{
    memset(n, 0, sizeof(*n));
    anv_loopback_attach(&bus, &n->port);
    anv_link_init(&n->link, anv_loopback_ops(), &n->port);
    anv_link_config_t lc;
    anv_link_config_default(&lc);
    lc.n_filters = anv_proto_node_filters(self, lc.filters);
    anv_link_start(&n->link, &lc);

    anv_isotp_config_t c;
    anv_isotp_config_default(&c);
    c.self = self;
    c.peer = peer;
    c.block_size = bs;
    c.st_min = st_min;
    c.rx_buf = n->rx_buf;
    c.rx_buf_size = rx_size;
    c.send = link_send;
    c.on_rx = on_rx;
    c.on_tx_done = on_tx_done;
    c.on_rx_error = on_rx_error;
    c.user = n;
    MT_CHECK(anv_isotp_init(&n->tp, &c) == ANV_ISOTP_OK);
}

static void setup(uint8_t bs, uint8_t st_min, uint16_t b_rx_size)
{
    anv_loopback_bus_init(&bus);
    anv_loopback_set_tap(&bus, tap, NULL);
    wire = WIRE_OK;
    wire_cf_index = 0;
    cf_seen = 0;
    holding = false;
    n_cf_times = 0;
    now_us = 1000000u;
    node_setup(&na, ID_A, ID_B, bs, st_min, ANV_ISOTP_MAX_LEN);
    node_setup(&nb, ID_B, ID_A, bs, st_min, b_rx_size);
    /* A third device, so traffic for others exists on the bus. */
    anv_loopback_attach(&bus, &other_port);
    anv_link_init(&other_link, anv_loopback_ops(), &other_port);
    anv_link_config_t lc;
    anv_link_config_default(&lc);
    anv_link_start(&other_link, &lc);
}

static void node_step(node_t *n)
{
    anv_link_frame_t f;
    while (anv_link_receive(&n->link, &f)) {
        (void)anv_isotp_on_frame(&n->tp, &f, now_us);
    }
    anv_isotp_poll(&n->tp, now_us);
}

/* Run both nodes for `ms`, polling every 100 us like a busy main loop. */
static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms * 10u; i++) {
        now_us += 100u;
        node_step(&na);
        node_step(&nb);
        anv_link_frame_t f;
        while (anv_link_receive(&other_link, &f)) {
        }
    }
}

static void fill(uint8_t *buf, uint16_t len, uint8_t seed)
{
    for (uint16_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(i * 7u + seed + (i >> 8));
    }
}

static uint8_t payload[ANV_ISOTP_MAX_LEN];
static uint8_t payload2[ANV_ISOTP_MAX_LEN];

/* --- tests ------------------------------------------------------------------------------ */
static void test_single_frame(void)
{
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    fill(payload, 7, 1);
    MT_CHECK(anv_isotp_send(&na.tp, payload, 7, now_us) == ANV_ISOTP_OK);
    run_ms(2);
    MT_CHECK(na.tx_done == 1 && na.tx_result == ANV_ISOTP_OK);
    MT_CHECK(nb.rx_count == 1 && nb.got_len == 7 && memcmp(nb.got, payload, 7) == 0);
}

static void test_long_transfers(void)
{
    const uint16_t sizes[] = {8, 13, 14, 100, 1000, 4095};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        setup(ANV_ISOTP_DEFAULT_BLOCK_SIZE, ANV_ISOTP_DEFAULT_ST_MIN, ANV_ISOTP_MAX_LEN);
        fill(payload, sizes[k], (uint8_t)k);
        MT_CHECK(anv_isotp_send(&na.tp, payload, sizes[k], now_us) == ANV_ISOTP_OK);
        run_ms(400);
        MT_CHECK(na.tx_done == 1 && na.tx_result == ANV_ISOTP_OK);
        MT_CHECK(nb.rx_count == 1 && nb.got_len == sizes[k]);
        MT_CHECK(memcmp(nb.got, payload, sizes[k]) == 0);
        MT_CHECK(nb.rx_errors == 0);
    }
}

static void test_block_size_and_both_directions(void)
{
    setup(4, 0, ANV_ISOTP_MAX_LEN);
    fill(payload, 2000, 3);
    fill(payload2, 3000, 9);
    MT_CHECK(anv_isotp_send(&na.tp, payload, 2000, now_us) == ANV_ISOTP_OK);
    MT_CHECK(anv_isotp_send(&nb.tp, payload2, 3000, now_us) == ANV_ISOTP_OK);
    MT_CHECK(anv_isotp_send(&na.tp, payload, 10, now_us) == ANV_ISOTP_ERR_BUSY);
    run_ms(200);
    MT_CHECK(na.tx_result == ANV_ISOTP_OK && nb.tx_result == ANV_ISOTP_OK);
    MT_CHECK(nb.got_len == 2000 && memcmp(nb.got, payload, 2000) == 0);
    MT_CHECK(na.got_len == 3000 && memcmp(na.got, payload2, 3000) == 0);
}

static void test_st_min_respected(void)
{
    setup(0, 0xF5, ANV_ISOTP_MAX_LEN); /* 500 us */
    fill(payload, 300, 5);
    anv_isotp_send(&na.tp, payload, 300, now_us);
    run_ms(100);
    MT_CHECK(nb.got_len == 300);
    MT_CHECK(n_cf_times > 10);
    for (int i = 1; i < n_cf_times; i++) {
        MT_CHECK(cf_times[i] - cf_times[i - 1] >= 500u);
    }
    MT_CHECK(anv_isotp_st_min_us(0x05) == 5000u);
    MT_CHECK(anv_isotp_st_min_us(0xF1) == 100u);
    MT_CHECK(anv_isotp_st_min_us(0x80) == 127000u); /* reserved: slowest */
}

static void test_lost_frame(void)
{
    /* Block size 0: the receiver notices, the sender can't (no end-to-end ack in ISO-TP). */
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    wire = WIRE_DROP_CF;
    wire_cf_index = 10;
    fill(payload, 500, 1);
    anv_isotp_send(&na.tp, payload, 500, now_us);
    run_ms(100);
    MT_CHECK(nb.rx_count == 0);
    MT_CHECK(nb.rx_errors == 1 && nb.rx_error == ANV_ISOTP_ERR_WRONG_SN);
    MT_CHECK(nb.tp.stats.rx_wrong_sn == 1);
    MT_CHECK(nb.tp.stats.rx_stray > 0); /* the rest of the transfer, ignored */

    /* Block size 8: the sender times out waiting for flow control. */
    setup(8, 0, ANV_ISOTP_MAX_LEN);
    wire = WIRE_DROP_CF;
    wire_cf_index = 3;
    anv_isotp_send(&na.tp, payload, 500, now_us);
    run_ms(ANV_ISOTP_DEFAULT_TIMEOUT_MS + 50u);
    MT_CHECK(nb.rx_error == ANV_ISOTP_ERR_WRONG_SN);
    MT_CHECK(na.tx_done == 1 && na.tx_result == ANV_ISOTP_ERR_TIMEOUT_FC);

    /* And the channel works again afterwards. */
    wire = WIRE_OK;
    anv_isotp_send(&na.tp, payload, 500, now_us);
    run_ms(100);
    MT_CHECK(na.tx_result == ANV_ISOTP_OK && nb.rx_count == 1 && memcmp(nb.got, payload, 500) == 0);
}

static void test_out_of_order_and_duplicate(void)
{
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    wire = WIRE_SWAP_CF;
    wire_cf_index = 5;
    fill(payload, 200, 2);
    anv_isotp_send(&na.tp, payload, 200, now_us);
    run_ms(50);
    MT_CHECK(nb.rx_count == 0 && nb.rx_error == ANV_ISOTP_ERR_WRONG_SN);

    setup(0, 0, ANV_ISOTP_MAX_LEN);
    wire = WIRE_DUP_CF;
    wire_cf_index = 2;
    anv_isotp_send(&na.tp, payload, 200, now_us);
    run_ms(50);
    MT_CHECK(nb.rx_count == 0 && nb.rx_error == ANV_ISOTP_ERR_WRONG_SN);

    /* Sequence numbers wrap 15 -> 0 in a long transfer without complaint. */
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    anv_isotp_send(&na.tp, payload, 200, now_us); /* 28 consecutive frames */
    run_ms(50);
    MT_CHECK(nb.rx_count == 1 && nb.rx_errors == 0);
}

static void test_last_frame_lost_times_out(void)
{
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    wire = WIRE_DROP_CF;
    wire_cf_index = 3; /* first frame + 3 consecutive frames: lose the last */
    fill(payload, 6 + 7 + 7 + 3, 4);
    anv_isotp_send(&na.tp, payload, 6 + 7 + 7 + 3, now_us);
    run_ms(ANV_ISOTP_DEFAULT_TIMEOUT_MS / 2u);
    MT_CHECK(anv_isotp_rx_busy(&nb.tp));
    run_ms(ANV_ISOTP_DEFAULT_TIMEOUT_MS);
    MT_CHECK(!anv_isotp_rx_busy(&nb.tp));
    MT_CHECK(nb.rx_errors == 1 && nb.rx_error == ANV_ISOTP_ERR_TIMEOUT_CF);
}

static void test_flow_control_lost(void)
{
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    wire = WIRE_DROP_FC;
    fill(payload, 100, 1);
    anv_isotp_send(&na.tp, payload, 100, now_us);
    run_ms(ANV_ISOTP_DEFAULT_TIMEOUT_MS - 10u);
    MT_CHECK(na.tx_done == 0 && anv_isotp_tx_busy(&na.tp));
    run_ms(20);
    MT_CHECK(na.tx_done == 1 && na.tx_result == ANV_ISOTP_ERR_TIMEOUT_FC);
}

static void test_overflow(void)
{
    setup(0, 0, 100);
    fill(payload, 500, 1);
    anv_isotp_send(&na.tp, payload, 500, now_us);
    run_ms(10);
    MT_CHECK(na.tx_done == 1 && na.tx_result == ANV_ISOTP_ERR_OVERFLOW);
    MT_CHECK(nb.rx_errors == 1 && nb.rx_error == ANV_ISOTP_ERR_OVERFLOW);
    anv_isotp_send(&na.tp, payload, 100, now_us);
    run_ms(10);
    MT_CHECK(na.tx_result == ANV_ISOTP_OK && nb.got_len == 100);
}

static void test_bad_arguments(void)
{
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    MT_CHECK(anv_isotp_send(&na.tp, payload, 0, now_us) == ANV_ISOTP_ERR_ARG);
    MT_CHECK(anv_isotp_send(&na.tp, payload, ANV_ISOTP_MAX_LEN + 1u, now_us) == ANV_ISOTP_ERR_ARG);
    MT_CHECK(anv_isotp_send(&na.tp, NULL, 5, now_us) == ANV_ISOTP_ERR_ARG);
    anv_isotp_config_t c;
    anv_isotp_config_default(&c);
    anv_isotp_t t;
    MT_CHECK(anv_isotp_init(&t, &c) == ANV_ISOTP_ERR_ARG); /* no callbacks or buffer */
}

static void test_foreign_frames_ignored(void)
{
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    const uint8_t sf[8] = {0x03, 1, 2, 3, 0xAA, 0xAA, 0xAA, 0xAA};
    anv_link_frame_t f;
    const anv_proto_addr_t wrong_src = {ANV_TRANSFER_PRIORITY, ID_B, ANV_TRANSFER_PORT, 33};
    const anv_proto_addr_t wrong_port = {ANV_TRANSFER_PRIORITY, ID_B, ANV_TRANSFER_PORT + 1u, ID_A};
    const anv_proto_addr_t wrong_dst = {ANV_TRANSFER_PRIORITY, 21, ANV_TRANSFER_PORT, ID_A};
    const anv_proto_addr_t right = {ANV_TRANSFER_PRIORITY, ID_B, ANV_TRANSFER_PORT, ID_A};
    anv_proto_frame(&f, &wrong_src, sf, 8);
    MT_CHECK(!anv_isotp_on_frame(&nb.tp, &f, now_us));
    anv_proto_frame(&f, &wrong_port, sf, 8);
    MT_CHECK(!anv_isotp_on_frame(&nb.tp, &f, now_us));
    anv_proto_frame(&f, &wrong_dst, sf, 8);
    MT_CHECK(!anv_isotp_on_frame(&nb.tp, &f, now_us));
    MT_CHECK(nb.rx_count == 0);
    anv_proto_frame(&f, &right, sf, 8);
    MT_CHECK(anv_isotp_on_frame(&nb.tp, &f, now_us));
    MT_CHECK(nb.rx_count == 1 && nb.got_len == 3);
    /* A consecutive frame out of nowhere is counted, not delivered. */
    const uint8_t cf[8] = {0x21, 1, 2, 3, 4, 5, 6, 7};
    anv_proto_frame(&f, &right, cf, 8);
    anv_isotp_on_frame(&nb.tp, &f, now_us);
    MT_CHECK(nb.tp.stats.rx_stray == 1 && nb.rx_count == 1);
}

static void test_wait_frames(void)
{
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    wire = WIRE_DROP_FC; /* B's own CTS never arrives; we play B's flow control by hand */
    fill(payload, 100, 1);
    anv_isotp_send(&na.tp, payload, 100, now_us);
    run_ms(1);
    const anv_proto_addr_t from_b = {ANV_TRANSFER_PRIORITY, ID_A, ANV_TRANSFER_PORT, ID_B};
    const uint8_t wait_fc[3] = {0x31, 0, 0};
    anv_link_frame_t f;
    anv_proto_frame(&f, &from_b, wait_fc, 3);
    /* Each WAIT restarts the flow-control timer... */
    for (unsigned i = 0; i < ANV_ISOTP_DEFAULT_MAX_WAIT; i++) {
        run_ms(ANV_ISOTP_DEFAULT_TIMEOUT_MS - 100u);
        anv_isotp_on_frame(&na.tp, &f, now_us);
    }
    MT_CHECK(na.tx_done == 0);
    /* ...up to max_wait of them. */
    anv_isotp_on_frame(&na.tp, &f, now_us);
    MT_CHECK(na.tx_done == 1 && na.tx_result == ANV_ISOTP_ERR_WAIT_LIMIT);
}

static void test_link_refusing_frames(void)
{
    /* Briefly full: frames wait and go out later. */
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    fill(payload, 300, 6);
    na.refuse = true;
    anv_isotp_send(&na.tp, payload, 300, now_us);
    run_ms(5);
    MT_CHECK(anv_isotp_tx_busy(&na.tp));
    na.refuse = false;
    run_ms(50);
    MT_CHECK(na.tx_result == ANV_ISOTP_OK && nb.got_len == 300 && memcmp(nb.got, payload, 300) == 0);

    /* Full mid-transfer, for good (e.g. bus-off): the sender gives up. */
    setup(0, 0xF5, ANV_ISOTP_MAX_LEN);
    anv_isotp_send(&na.tp, payload, 300, now_us);
    run_ms(3);
    na.refuse = true;
    run_ms(ANV_ISOTP_DEFAULT_TIMEOUT_MS + 10u);
    MT_CHECK(na.tx_done == 1 && na.tx_result == ANV_ISOTP_ERR_TX_STALLED);
    MT_CHECK(nb.rx_error == ANV_ISOTP_ERR_TIMEOUT_CF);

    /* A flow control that couldn't go out is retried by poll. */
    setup(0, 0, ANV_ISOTP_MAX_LEN);
    nb.refuse = true;
    anv_isotp_send(&na.tp, payload, 300, now_us);
    run_ms(5);
    MT_CHECK(nb.tp.rx_fc_pending);
    nb.refuse = false;
    run_ms(50);
    MT_CHECK(na.tx_result == ANV_ISOTP_OK && nb.got_len == 300);
}

static void test_abort(void)
{
    setup(0, 0x7F, ANV_ISOTP_MAX_LEN); /* slow: 127 ms per frame */
    fill(payload, 300, 6);
    anv_isotp_send(&na.tp, payload, 300, now_us);
    run_ms(300);
    MT_CHECK(anv_isotp_tx_busy(&na.tp) && anv_isotp_rx_busy(&nb.tp));
    anv_isotp_abort(&na.tp);
    anv_isotp_abort(&nb.tp);
    MT_CHECK(na.tx_result == ANV_ISOTP_ERR_ABORTED && nb.rx_error == ANV_ISOTP_ERR_ABORTED);
    MT_CHECK(!anv_isotp_tx_busy(&na.tp) && !anv_isotp_rx_busy(&nb.tp));
    MT_CHECK(anv_isotp_result_name(ANV_ISOTP_ERR_WRONG_SN)[0] != '?');
}

int main(void)
{
    MT_RUN(test_single_frame);
    MT_RUN(test_long_transfers);
    MT_RUN(test_block_size_and_both_directions);
    MT_RUN(test_st_min_respected);
    MT_RUN(test_lost_frame);
    MT_RUN(test_out_of_order_and_duplicate);
    MT_RUN(test_last_frame_lost_times_out);
    MT_RUN(test_flow_control_lost);
    MT_RUN(test_overflow);
    MT_RUN(test_bad_arguments);
    MT_RUN(test_foreign_frames_ignored);
    MT_RUN(test_wait_frames);
    MT_RUN(test_link_refusing_frames);
    MT_RUN(test_abort);
    return mt_summary();
}
