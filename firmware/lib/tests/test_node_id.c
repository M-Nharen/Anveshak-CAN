/* anv_node: the node ID comes from the application (DIP switches); duplicates are detected. */
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

static void test_id_is_required(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type); /* node_id left at 0 */
    MT_CHECK(!h_start(0));
    h_config(0, &anv_esp32_bench_type)->node_id = ANV_NODE_UNASSIGNED; /* 255: reserved */
    MT_CHECK(!h_start(0));
    h_config(0, &anv_esp32_bench_type)->node_id = ANV_NODE_BROADCAST;
    MT_CHECK(!h_start(0));
}

static void test_dip_id_used_at_once(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type)->node_id = ID;
    MT_CHECK(h_start(0));
    anv_node_t *n = &h_nodes[0].node;
    MT_CHECK(anv_node_id(n) == ID);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE); /* straight to idle: nothing to wait for */
    h_step(1);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, ID) == 1); /* announces itself on the first poll */
    h_step(1000);
    for (int i = 0; i < h_log_n; i++) {
        MT_CHECK(anv_id_src(h_log[i].id) == ID); /* every frame it sends carries its ID */
    }
    /* A reboot keeps it (it's the application's, not something stored). */
    MT_CHECK(h_start(0));
    MT_CHECK(anv_node_id(n) == ID);
}

static void test_duplicate_id(void)
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
    /* The board with its own ID is untouched. */
    MT_CHECK(!anv_node_id_conflict(&h_nodes[2].node));
    MT_CHECK(anv_node_state(&h_nodes[2].node) == ANV_NODE_STATE_IDLE);

    /* Both conflicted boards go quiet: no more heartbeats from that ID. */
    h_clear_log();
    h_step_with_leader(500, TERM);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, ID) == 0);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, ID + 1) >= 4);

    /* fault_reset can't clear it, and it can't be armed: only a reset (with fixed switches) does. */
    h_nmt(ID, ANV_NMT_COMMAND_FAULT_RESET, TERM);
    h_nmt(ID, ANV_NMT_COMMAND_ARM, TERM);
    h_step_with_leader(5, TERM);
    MT_CHECK(anv_node_state(&h_nodes[0].node) == ANV_NODE_STATE_FAULT);
    MT_CHECK(!anv_node_armed(&h_nodes[0].node));

    /* E-stop still gets out of a conflicted board. */
    h_clear_log();
    anv_node_estop(&h_nodes[0].node, 9);
    h_step(1);
    MT_CHECK(h_count(ANV_ESTOP_PORT, ID) == 1);
    MT_CHECK(h_nodes[2].node.estop_latched);
}

int main(void)
{
    MT_RUN(test_id_is_required);
    MT_RUN(test_dip_id_used_at_once);
    MT_RUN(test_duplicate_id);
    return mt_summary();
}
