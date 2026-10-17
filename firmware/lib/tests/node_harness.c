#include "node_harness.h"

#include <string.h>

anv_loopback_bus_t h_bus;
h_node_t h_nodes[H_MAX_NODES];
uint32_t h_now;
anv_link_frame_t h_log[H_LOG];
int h_log_n;

static anv_loopback_port_t host_port;
static anv_link_t host_link;
static anv_node_config_t cfgs[H_MAX_NODES];
static bool started[H_MAX_NODES];
static uint8_t txn;

static anv_isotp_t h_transfer;
static uint8_t h_transfer_rx_buf[H_TRANSFER_RX_CAP];
static bool h_transfer_tx_done;
static anv_isotp_result_t h_transfer_tx_result;
h_transfer_rx_t h_transfer_last_rx;

static h_app_t *app(void *user)
{
    return (h_app_t *)user;
}

/* ------------------------------------------------------------------------------------------ */
/* a fake OTA backend, one per node slot (h_node_t::ota)                                      */

static bool ota_begin(void *ctx, uint32_t image_len)
{
    h_ota_t *o = (h_ota_t *)ctx;
    if (o->capacity != 0u && image_len > o->capacity) {
        return false;
    }
    o->active = true;
    o->written = 0;
    return true;
}

static bool ota_write(void *ctx, const uint8_t *data, uint16_t len)
{
    (void)data;
    h_ota_t *o = (h_ota_t *)ctx;
    if (!o->active || o->fail_writes) {
        return false;
    }
    o->written += len;
    return true;
}

static bool ota_commit(void *ctx)
{
    h_ota_t *o = (h_ota_t *)ctx;
    if (!o->active) {
        return false;
    }
    o->active = false;
    o->active_version = (uint8_t)(1u - o->active_version);
    o->commits++;
    return true;
}

static void ota_abort(void *ctx)
{
    h_ota_t *o = (h_ota_t *)ctx;
    o->active = false;
    o->aborts++;
}

static void ota_confirm(void *ctx)
{
    ((h_ota_t *)ctx)->confirms++;
}

static void ota_rollback_and_reboot(void *ctx)
{
    /* A real backend never returns from this; the fake one does, so the test can see it ran. */
    h_ota_t *o = (h_ota_t *)ctx;
    o->active_version = (uint8_t)(1u - o->active_version);
    o->rollbacks++;
}

static const anv_node_ota_backend_t h_ota_backend = {
    ota_begin, ota_write, ota_commit, ota_abort, ota_confirm, ota_rollback_and_reboot,
};

const anv_node_ota_backend_t *h_ota_ops(void)
{
    return &h_ota_backend;
}

static void outputs_off(anv_node_t *n, void *u)
{
    (void)n;
    app(u)->outputs_off++;
}

static void setup(anv_node_t *n, void *u)
{
    (void)n;
    app(u)->setups++;
}

static void loop(anv_node_t *n, uint32_t now, void *u)
{
    (void)n;
    (void)now;
    app(u)->loops++;
}

static void on_command(anv_node_t *n, const anv_msg_desc_t *m, const uint8_t *d, uint8_t len, void *u)
{
    (void)n;
    app(u)->commands++;
    app(u)->last_command_port = m->port;
    memcpy(app(u)->last_command, d, len);
}

static bool fill_stream(anv_node_t *n, const anv_msg_desc_t *m, uint8_t out[8], void *u)
{
    (void)n;
    app(u)->streams_filled++;
    out[0] = (uint8_t)m->port;
    return true;
}

static void on_setting(anv_node_t *n, const anv_param_desc_t *p, anv_value_t v, void *u)
{
    (void)n;
    (void)v;
    app(u)->setting_changes++;
    app(u)->last_setting = p->index;
}

static uint8_t on_action(anv_node_t *n, const anv_action_desc_t *a, anv_value_t arg, void *u)
{
    (void)n;
    app(u)->actions++;
    app(u)->last_action = a->index;
    app(u)->last_action_arg = arg.u;
    return ANV_SERVICE_STATUS_OK;
}

static void on_sync(anv_node_t *n, uint8_t counter, void *u)
{
    (void)n;
    (void)counter;
    app(u)->syncs++;
}

static void on_identify(anv_node_t *n, void *u)
{
    (void)n;
    app(u)->identifies++;
}

static void on_reset(anv_node_t *n, void *u)
{
    (void)n;
    app(u)->resets++;
}

static void on_frame(anv_node_t *n, const anv_link_frame_t *f, void *u)
{
    (void)n;
    (void)f;
    app(u)->raw_frames++;
}

void h_reset(void)
{
    memset(h_nodes, 0, sizeof h_nodes);
    memset(started, 0, sizeof started);
    h_now = 1000;
    h_log_n = 0;
    memset(&h_transfer_last_rx, 0, sizeof h_transfer_last_rx);
    anv_loopback_bus_init(&h_bus);
    anv_loopback_attach(&h_bus, &host_port);
    anv_link_init(&host_link, anv_loopback_ops(), &host_port);
    anv_link_config_t lc;
    anv_link_config_default(&lc);
    anv_link_start(&host_link, &lc);
    h_transfer_bind(ANV_NODE_BROADCAST);
    for (int i = 0; i < H_MAX_NODES; i++) {
        h_node_t *h = &h_nodes[i];
        anv_loopback_attach(&h_bus, &h->port);
        anv_link_init(&h->link, anv_loopback_ops(), &h->port);
        anv_link_start(&h->link, &lc);
        anv_node_memstore_init(&h->store);
    }
}

anv_node_config_t *h_config(int i, const anv_type_desc_t *type)
{
    h_node_t *h = &h_nodes[i];
    anv_node_config_t *c = &cfgs[i];
    anv_node_config_default(c);
    c->type = type;
    c->link = &h->link;
    const uint8_t mac[6] = {2, 0, 0, 0, 0, (uint8_t)(i + 1)};
    memcpy(c->mac, mac, 6);
    c->storage = anv_node_memstore_ops();
    c->storage_ctx = &h->store;
    c->ota = h_ota_ops();
    c->ota_ctx = &h->ota;
    c->user = &h->app;
    c->outputs_off = outputs_off;
    c->setup = setup;
    c->loop = loop;
    c->on_command = on_command;
    c->fill_stream = fill_stream;
    c->on_setting_changed = on_setting;
    c->on_action = on_action;
    c->on_sync = on_sync;
    c->on_identify = on_identify;
    c->on_reset = on_reset;
    c->on_frame = on_frame;
    c->info.fw_build = 0x1234;
    return c;
}

bool h_start(int i)
{
    h_node_t *h = &h_nodes[i];
    memset(&h->app, 0, sizeof h->app);
    if (!anv_node_init(&h->node, &cfgs[i])) {
        return false;
    }
    anv_node_begin(&h->node, h_now);
    started[i] = true;
    return true;
}

static void poll_host(void)
{
    anv_link_frame_t f;
    while (anv_link_receive(&host_link, &f)) {
        if (h_log_n < H_LOG) {
            h_log[h_log_n++] = f;
        }
        if (anv_id_port(f.id) == ANV_TRANSFER_PORT) {
            (void)anv_isotp_on_frame(&h_transfer, &f, h_now * 1000u);
        }
    }
}

void h_step(uint32_t ms)
{
    for (uint32_t k = 0; k < ms; k++) {
        h_now++;
        for (int i = 0; i < H_MAX_NODES; i++) {
            if (started[i]) {
                anv_node_poll(&h_nodes[i].node, h_now);
            }
        }
        poll_host();
        anv_isotp_poll(&h_transfer, h_now * 1000u);
    }
}

static bool h_transfer_send_frame(void *user, const anv_link_frame_t *frame)
{
    (void)user;
    anv_link_frame_t f = *frame;
    return anv_link_send(&host_link, &f) == ANV_LINK_OK;
}

static void h_transfer_on_rx(void *user, const uint8_t *data, uint16_t len)
{
    (void)user;
    h_transfer_last_rx.peer = h_transfer.cfg.peer;
    h_transfer_last_rx.len = len > H_TRANSFER_RX_CAP ? H_TRANSFER_RX_CAP : len;
    memcpy(h_transfer_last_rx.data, data, h_transfer_last_rx.len);
    h_transfer_last_rx.got = true;
}

static void h_transfer_on_tx_done(void *user, anv_isotp_result_t r)
{
    (void)user;
    h_transfer_tx_done = true;
    h_transfer_tx_result = r;
}

void h_transfer_bind(anv_node_id_t peer)
{
    anv_isotp_config_t c;
    anv_isotp_config_default(&c);
    c.self = H_HOST_ID;
    c.peer = peer;
    c.rx_buf = h_transfer_rx_buf;
    c.rx_buf_size = (uint16_t)sizeof h_transfer_rx_buf;
    c.send = h_transfer_send_frame;
    c.on_rx = h_transfer_on_rx;
    c.on_tx_done = h_transfer_on_tx_done;
    (void)anv_isotp_init(&h_transfer, &c);
}

anv_isotp_result_t h_transfer_send(const uint8_t *data, uint16_t len)
{
    h_transfer_tx_done = false;
    (void)anv_isotp_send(&h_transfer, data, len, h_now * 1000u);
    for (int t = 0; t < 2000 && !h_transfer_tx_done; t++) {
        h_step(1);
    }
    return h_transfer_tx_done ? h_transfer_tx_result : ANV_ISOTP_ERR_TIMEOUT_FC;
}

void h_send(uint8_t priority, anv_node_id_t dst, uint16_t port, const uint8_t *data, uint8_t len)
{
    anv_link_frame_t f;
    const anv_proto_addr_t a = {priority, dst, port, H_HOST_ID};
    anv_proto_frame(&f, &a, data, len);
    anv_link_send(&host_link, &f);
}

void h_host_heartbeat(anv_node_id_t src, uint8_t role, uint16_t term)
{
    anv_host_heartbeat_t hb;
    memset(&hb, 0, sizeof hb);
    hb.role = role;
    hb.term = term;
    hb.leader = src;
    uint8_t d[8];
    const uint8_t len = anv_host_heartbeat_pack(d, &hb);
    anv_link_frame_t f;
    const anv_proto_addr_t a = {ANV_HOST_HEARTBEAT_PRIORITY, ANV_NODE_BROADCAST, ANV_HOST_HEARTBEAT_PORT, src};
    anv_proto_frame(&f, &a, d, len);
    anv_link_send(&host_link, &f);
}

void h_nmt(anv_node_id_t dst, uint8_t command, uint16_t term)
{
    const anv_nmt_t m = {command, term};
    uint8_t d[8];
    const uint8_t len = anv_nmt_pack(d, &m);
    h_send(ANV_NMT_PRIORITY, dst, ANV_NMT_PORT, d, len);
}

void h_estop(anv_node_id_t src)
{
    const anv_estop_t e = {0xE5, 7};
    uint8_t d[8];
    const uint8_t len = anv_estop_pack(d, &e);
    anv_link_frame_t f;
    const anv_proto_addr_t a = {ANV_ESTOP_PRIORITY, ANV_NODE_BROADCAST, ANV_ESTOP_PORT, src};
    anv_proto_frame(&f, &a, d, len);
    anv_link_send(&host_link, &f);
}

bool h_service(anv_node_id_t dst, uint8_t op, uint16_t index, uint32_t value, anv_service_t *resp)
{
    anv_service_t r;
    memset(&r, 0, sizeof r);
    r.op = op;
    r.txn = ++txn;
    r.index = index;
    r.value = value;
    uint8_t d[8];
    const uint8_t len = anv_service_pack(d, &r);
    const int from = h_log_n;
    h_send(ANV_SERVICE_PRIORITY, dst, ANV_SERVICE_PORT, d, len);
    for (int t = 0; t < 50; t++) {
        h_step(1);
        for (int i = from; i < h_log_n; i++) {
            const anv_link_frame_t *f = &h_log[i];
            if (anv_id_port(f->id) == ANV_SERVICE_PORT && anv_id_src(f->id) == dst &&
                anv_id_dst(f->id) == H_HOST_ID && anv_service_unpack(resp, f->data, f->len) && resp->txn == txn) {
                return true;
            }
        }
    }
    return false;
}

void h_step_with_leader(uint32_t ms, uint16_t term)
{
    for (uint32_t k = 0; k < ms; k++) {
        if (h_now % 50u == 0u) {
            h_host_heartbeat(H_HOST_ID, ANV_HOST_ROLE_LEADER, term);
        }
        h_step(1);
    }
}

int h_count(uint16_t port, anv_node_id_t src)
{
    int n = 0;
    for (int i = 0; i < h_log_n; i++) {
        if (anv_id_port(h_log[i].id) == port && anv_id_src(h_log[i].id) == src) {
            n++;
        }
    }
    return n;
}

const anv_link_frame_t *h_last(uint16_t port, anv_node_id_t src)
{
    for (int i = h_log_n - 1; i >= 0; i--) {
        if (anv_id_port(h_log[i].id) == port && anv_id_src(h_log[i].id) == src) {
            return &h_log[i];
        }
    }
    return NULL;
}

void h_clear_log(void)
{
    h_log_n = 0;
}
