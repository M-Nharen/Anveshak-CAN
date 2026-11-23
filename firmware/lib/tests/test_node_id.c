/* anv_node: where the node ID comes from (fixed in the sketch, DIP switches, or the hosts) and
 * what happens when two boards share one. */
#include <string.h>

#include "anv_esp32_bench.h"
#include "minitest.h"
#include "node_harness.h"

#define ID 12
#define TERM 3

/* The first event frame node `src` sent with this code, or NULL. */
static bool sent_event(anv_node_id_t src, uint16_t code, anv_event_t *out)
{
    for (int i = 0; i < h_log_n; i++) {
        const anv_link_frame_t *f = &h_log[i];
        anv_event_t e;
        if (anv_id_port(f->id) == ANV_EVENT_PORT && anv_id_src(f->id) == src &&
            anv_event_unpack(&e, f->data, f->len) && e.code == code) {
            if (out != NULL) {
                *out = e;
            }
            return true;
        }
    }
    return false;
}

/* What the hosts' ID service would send: node i's MAC gets `id`. */
static void assign(int i, anv_node_id_t id, bool move)
{
    anv_id_assign_t a;
    memcpy(a.mac, h_nodes[i].node.cfg.mac, 6);
    a.node_id = id;
    a.flags = move ? 1u : 0u;
    uint8_t d[8];
    const uint8_t len = anv_id_assign_pack(d, &a);
    h_send(ANV_ID_ASSIGN_PRIORITY, ANV_NODE_UNASSIGNED, ANV_ID_ASSIGN_PORT, d, len);
}

/* Fake DIP switches. */
static int dip_reads;
static uint16_t dip_value;
static uint16_t read_dip(void *ctx)
{
    (void)ctx;
    dip_reads++;
    return dip_value;
}

static void with_dip(int i, uint16_t value)
{
    anv_node_config_t *c = h_config(i, &anv_esp32_bench_type);
    c->read_dip = read_dip;
    dip_value = value;
    dip_reads = 0;
}

static void test_bad_config(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type)->node_id = ANV_NODE_UNASSIGNED; /* 255: reserved */
    MT_CHECK(!h_start(0));

    /* A sketch that carries on after a failed init doesn't crash and doesn't touch the bus:
     * begin and poll do nothing, and so do the calls it might still make. */
    anv_node_config_t *c = h_config(0, &anv_esp32_bench_type);
    c->node_id = ANV_NODE_UNASSIGNED;
    anv_node_t *n = &h_nodes[0].node;
    MT_CHECK(!anv_node_init(n, c));
    anv_node_begin(n, h_now);
    for (uint32_t t = 1; t <= 500; t++) {
        anv_node_poll(n, h_now + t);
    }
    const uint8_t data[1] = {0};
    anv_node_estop(n, 1);
    anv_node_event(n, 0x4001, 0);
    anv_node_event_clear(n, 0x4001, 0);
    anv_node_set_fault(n, true);
    anv_node_disarm(n);
    MT_CHECK(!anv_node_forget_id(n));
    MT_CHECK(!anv_node_send(n, 64, data, 1));
    MT_CHECK(anv_node_set(n, ANV_PARAM_DEBUG_LEVEL, (anv_value_t){.u = 1}) == ANV_SERVICE_STATUS_FAILED);
    MT_CHECK(anv_node_save(n) == ANV_SERVICE_STATUS_FAILED);
    MT_CHECK(anv_node_restore_defaults(n) == ANV_SERVICE_STATUS_FAILED);
    MT_CHECK(anv_node_set_stream_rate(n, 64, 1000) == ANV_SERVICE_STATUS_FAILED);
    h_step(1);
    MT_CHECK(h_log_n == 0);
    MT_CHECK(!n->begun);
}

static void test_fixed_id(void)
{
    h_reset();
    with_dip(0, 7);
    h_config(0, &anv_esp32_bench_type)->node_id = ID;
    MT_CHECK(h_start(0));
    anv_node_t *n = &h_nodes[0].node;
    MT_CHECK(anv_node_id(n) == ID && anv_node_id_origin(n) == ANV_NODE_ID_FIXED);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE);
    h_step(1);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, ID) == 1); /* announces itself on the first poll */
    h_step(1000);
    for (int i = 0; i < h_log_n; i++) {
        MT_CHECK(anv_id_src(h_log[i].id) == ID);
    }
    /* A host can't move it. */
    assign(0, 40, true);
    h_step(5);
    MT_CHECK(anv_node_id(n) == ID);
    MT_CHECK(!anv_node_forget_id(n));
}

static void test_auto_id_from_hosts(void)
{
    h_reset();
    with_dip(0, 5); /* switches present but id_source is auto (default): never read */
    MT_CHECK(h_start(0));
    anv_node_t *n = &h_nodes[0].node;
    MT_CHECK(dip_reads == 0);
    MT_CHECK(anv_node_id(n) == ANV_NODE_UNASSIGNED && anv_node_id_origin(n) == ANV_NODE_ID_NONE);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_UNASSIGNED);

    /* Asks from 255 with its MAC and type, and sends nothing else. */
    h_step(2000);
    MT_CHECK(h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED) >= 3);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, ANV_NODE_UNASSIGNED) == 0);
    anv_id_request_t r;
    MT_CHECK(anv_id_request_unpack(&r, h_last(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED)->data, 8));
    MT_CHECK(memcmp(r.mac, n->cfg.mac, 6) == 0 && r.type_id == anv_esp32_bench_type.type_id);
    /* Not addressable meanwhile: a service request to 255 gets no answer. */
    anv_service_t resp;
    MT_CHECK(!h_service(ANV_NODE_UNASSIGNED, ANV_SERVICE_OP_GET, ANV_PARAM_HEARTBEAT_HZ, 0, &resp));

    /* An answer for another board is ignored; ours is taken. */
    anv_id_assign_t other = {{9, 9, 9, 9, 9, 9}, 30, 0};
    uint8_t d[8];
    h_send(ANV_ID_ASSIGN_PRIORITY, ANV_NODE_UNASSIGNED, ANV_ID_ASSIGN_PORT, d, anv_id_assign_pack(d, &other));
    h_step(2);
    MT_CHECK(anv_node_id(n) == ANV_NODE_UNASSIGNED);
    assign(0, 20, false);
    h_step(2);
    MT_CHECK(anv_node_id(n) == 20 && anv_node_id_origin(n) == ANV_NODE_ID_AUTO);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE);
    h_clear_log();
    h_step(500);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, 20) >= 4);
    MT_CHECK(h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED) == 0);
    MT_CHECK(h_service(20, ANV_SERVICE_OP_GET, ANV_PARAM_HEARTBEAT_HZ, 0, &resp));

    /* A second plain answer (another host) doesn't move it. */
    assign(0, 21, false);
    h_step(2);
    MT_CHECK(anv_node_id(n) == 20);

    /* Saved: a reboot comes straight back as 20, without asking. */
    h_clear_log();
    MT_CHECK(h_start(0));
    MT_CHECK(anv_node_id(n) == 20 && anv_node_state(n) == ANV_NODE_STATE_IDLE);
    h_step(1000);
    MT_CHECK(h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED) == 0);
    MT_CHECK(dip_reads == 0);
}

static void test_move_and_forget(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type);
    MT_CHECK(h_start(0));
    anv_node_t *n = &h_nodes[0].node;
    h_step(1000);
    assign(0, 20, false);
    h_step(2);
    MT_CHECK(anv_node_id(n) == 20);

    /* Move while armed: refused. */
    h_step_with_leader(100, TERM);
    h_nmt(20, ANV_NMT_COMMAND_ARM, TERM);
    h_step_with_leader(5, TERM);
    MT_CHECK(anv_node_armed(n));
    assign(0, 7, true);
    h_step(2);
    MT_CHECK(anv_node_id(n) == 20);

    /* Disarmed: moved, saved, and it now answers only at 7. */
    h_nmt(20, ANV_NMT_COMMAND_DISARM, TERM);
    h_step_with_leader(5, TERM);
    assign(0, 7, true);
    h_step(2);
    MT_CHECK(anv_node_id(n) == 7);
    anv_service_t resp;
    MT_CHECK(h_service(7, ANV_SERVICE_OP_GET, ANV_PARAM_HEARTBEAT_HZ, 0, &resp));
    MT_CHECK(!h_service(20, ANV_SERVICE_OP_GET, ANV_PARAM_HEARTBEAT_HZ, 0, &resp));
    MT_CHECK(h_start(0) && anv_node_id(n) == 7);

    /* NMT forget_id: back to asking, nothing saved. */
    h_step_with_leader(100, TERM);
    h_nmt(7, ANV_NMT_COMMAND_FORGET_ID, TERM);
    h_step_with_leader(5, TERM);
    MT_CHECK(anv_node_id(n) == ANV_NODE_UNASSIGNED);
    MT_CHECK(h_start(0) && anv_node_id(n) == ANV_NODE_UNASSIGNED);
}

static void test_dip_source(void)
{
    /* id_source = dip (saved, as `anv-host set ... id_source=dip --save` does): switches used. */
    h_reset();
    with_dip(0, 5);
    MT_CHECK(h_start(0));
    anv_node_t *n = &h_nodes[0].node;
    MT_CHECK(anv_node_set(n, ANV_PARAM_ID_SOURCE, (anv_value_t){.u = ANV_ID_SOURCE_DIP}) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(anv_node_save(n) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(anv_node_id(n) == ANV_NODE_UNASSIGNED); /* takes effect after a reset */
    MT_CHECK(h_start(0));
    MT_CHECK(dip_reads == 1);
    MT_CHECK(anv_node_id(n) == 5 && anv_node_id_origin(n) == ANV_NODE_ID_DIP);
    h_step(1);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, 5) == 1);
    assign(0, 40, true); /* a host can't move a DIP ID */
    h_step(2);
    MT_CHECK(anv_node_id(n) == 5);

    /* Switches at 0 (or nothing on those pins): falls back to asking, and says so. */
    dip_value = 0;
    h_clear_log();
    MT_CHECK(h_start(0));
    MT_CHECK(anv_node_id(n) == ANV_NODE_UNASSIGNED);
    /* in the black box (it can't go on the bus yet: no ID) */
    bool logged = false;
    for (uint8_t i = 0; i < n->diag.count; i++) {
        logged = logged || (n->diag.entries[i].code == ANV_EVENT_DIP_ID_INVALID && n->diag.entries[i].detail == 0);
    }
    MT_CHECK(logged);
    h_step(1000);
    MT_CHECK(h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED) >= 1);
    assign(0, 18, false);
    h_step(2);
    MT_CHECK(anv_node_id(n) == 18 && anv_node_id_origin(n) == ANV_NODE_ID_AUTO);

    /* With the switches fixed, the next boot uses them again. */
    dip_value = 3;
    MT_CHECK(h_start(0) && anv_node_id(n) == 3);

    /* dip_by_default: same thing without setting it per board. */
    h_reset();
    with_dip(1, 9);
    anv_node_config_t *c = h_config(1, &anv_esp32_bench_type);
    c->read_dip = read_dip;
    c->dip_by_default = true;
    MT_CHECK(h_start(1) && anv_node_id(&h_nodes[1].node) == 9);
    MT_CHECK(!h_nodes[1].node.unsaved);
}

static void test_duplicate_fixed_id(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type)->node_id = ID;
    h_config(1, &anv_esp32_bench_type)->node_id = ID; /* same DIP setting */
    h_config(2, &anv_esp32_bench_type)->node_id = ID + 1;
    MT_CHECK(h_start(0) && h_start(1) && h_start(2));
    h_step_with_leader(300, TERM);

    for (int i = 0; i < 2; i++) {
        anv_node_t *n = &h_nodes[i].node;
        MT_CHECK(anv_node_id_conflict(n));
        MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_FAULT);
        MT_CHECK(n->stats.id_conflicts >= 1);
    }
    anv_event_t e;
    MT_CHECK(sent_event(ID, ANV_EVENT_ID_CONFLICT, &e));
    MT_CHECK(e.detail == ID && e.severity == ANV_SEVERITY_CRITICAL);
    MT_CHECK(!anv_node_id_conflict(&h_nodes[2].node));
    MT_CHECK(anv_node_state(&h_nodes[2].node) == ANV_NODE_STATE_IDLE);

    /* Both go quiet. */
    h_clear_log();
    h_step_with_leader(500, TERM);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, ID) == 0);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, ID + 1) >= 4);

    /* fault_reset can't clear it, and it can't be armed. */
    h_nmt(ID, ANV_NMT_COMMAND_FAULT_RESET, TERM);
    h_nmt(ID, ANV_NMT_COMMAND_ARM, TERM);
    h_step_with_leader(5, TERM);
    MT_CHECK(anv_node_state(&h_nodes[0].node) == ANV_NODE_STATE_FAULT);
    MT_CHECK(!anv_node_armed(&h_nodes[0].node));

    /* E-stop still gets out. */
    h_clear_log();
    anv_node_estop(&h_nodes[0].node, 9);
    h_step(1);
    MT_CHECK(h_count(ANV_ESTOP_PORT, ID) == 1);
    MT_CHECK(h_nodes[2].node.estop_latched);
}

static void test_duplicate_auto_id(void)
{
    /* Two boards end up with the same auto ID (say a lost ID table handed it out twice): both
     * drop it, forget the saved copy and ask again, instead of going quiet. */
    h_reset();
    h_config(0, &anv_esp32_bench_type);
    h_config(1, &anv_esp32_bench_type);
    MT_CHECK(h_start(0) && h_start(1));
    h_step(1000);
    assign(0, 20, false);
    assign(1, 20, false);
    h_step(300);
    for (int i = 0; i < 2; i++) {
        anv_node_t *n = &h_nodes[i].node;
        MT_CHECK(anv_node_id(n) == ANV_NODE_UNASSIGNED);
        MT_CHECK(!anv_node_id_conflict(n));
        MT_CHECK(n->stats.id_conflicts >= 1);
    }
    MT_CHECK(sent_event(20, ANV_EVENT_ID_CONFLICT, NULL));
    h_clear_log();
    h_step(1000);
    MT_CHECK(h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED) >= 2);
    /* Two boards asking at once from 255 is not a conflict: the counts don't grow. */
    const uint32_t c0 = h_nodes[0].node.stats.id_conflicts;
    h_step(1000);
    MT_CHECK(h_nodes[0].node.stats.id_conflicts == c0);
    assign(0, 20, false);
    assign(1, 21, false);
    h_step(300);
    MT_CHECK(anv_node_id(&h_nodes[0].node) == 20 && anv_node_id(&h_nodes[1].node) == 21);
    MT_CHECK(anv_node_state(&h_nodes[0].node) == ANV_NODE_STATE_IDLE);
    MT_CHECK(anv_node_state(&h_nodes[1].node) == ANV_NODE_STATE_IDLE);
}

int main(void)
{
    MT_RUN(test_bad_config);
    MT_RUN(test_fixed_id);
    MT_RUN(test_auto_id_from_hosts);
    MT_RUN(test_move_and_forget);
    MT_RUN(test_dip_source);
    MT_RUN(test_duplicate_fixed_id);
    MT_RUN(test_duplicate_auto_id);
    return mt_summary();
}
