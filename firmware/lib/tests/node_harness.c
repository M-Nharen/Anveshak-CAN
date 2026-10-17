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

static h_app_t *app(void *user)
{
    return (h_app_t *)user;
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
    anv_loopback_bus_init(&h_bus);
    anv_loopback_attach(&h_bus, &host_port);
    anv_link_init(&host_link, anv_loopback_ops(), &host_port);
    anv_link_config_t lc;
    anv_link_config_default(&lc);
    anv_link_start(&host_link, &lc);
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
    }
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
