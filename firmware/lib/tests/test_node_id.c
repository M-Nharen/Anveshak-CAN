/* anv_node: automatic ID assignment, saved and fixed IDs. */
#include <string.h>

#include "anv_esp32_bench.h"
#include "minitest.h"
#include "node_harness.h"

static void assign(int node_index, anv_node_id_t id)
{
    anv_id_assign_t a;
    memset(&a, 0, sizeof a);
    memcpy(a.mac, h_nodes[node_index].node.cfg.mac, 6);
    a.node_id = id;
    uint8_t d[8];
    const uint8_t len = anv_id_assign_pack(d, &a);
    h_send(ANV_ID_ASSIGN_PRIORITY, ANV_NODE_UNASSIGNED, ANV_ID_ASSIGN_PORT, d, len);
}

static void test_asks_until_answered(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type);
    MT_CHECK(h_start(0));
    anv_node_t *n = &h_nodes[0].node;
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_UNASSIGNED);
    MT_CHECK(anv_node_id(n) == ANV_NODE_UNASSIGNED);
    MT_CHECK(h_nodes[0].app.outputs_off >= 1); /* off from the start */
    MT_CHECK(h_nodes[0].app.setups == 1);

    h_step(600);
    MT_CHECK(h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED) >= 1); /* first one within 600 ms */
    h_step(2000);
    const int asked = h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED);
    MT_CHECK(asked >= 4 && asked <= 7); /* every 400-650 ms */
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, ANV_NODE_UNASSIGNED) == 0); /* no heartbeat without an ID */

    anv_id_request_t r;
    const anv_link_frame_t *f = h_last(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED);
    MT_CHECK(f != NULL && anv_id_request_unpack(&r, f->data, f->len));
    MT_CHECK(memcmp(r.mac, n->cfg.mac, 6) == 0);
    MT_CHECK(r.type_id == ANV_ESP32_BENCH_TYPE_ID);
    MT_CHECK(anv_id_dst(f->id) == ANV_NODE_BROADCAST);
}

static void test_assignment(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type);
    h_config(1, &anv_esp32_bench_type);
    MT_CHECK(h_start(0) && h_start(1));
    h_step(700);

    /* Someone else's MAC, and IDs nobody may have, change nothing. */
    assign(1, 20);
    h_step(5);
    MT_CHECK(anv_node_id(&h_nodes[0].node) == ANV_NODE_UNASSIGNED);
    MT_CHECK(anv_node_id(&h_nodes[1].node) == 20);
    assign(0, ANV_NODE_BROADCAST);
    assign(0, ANV_NODE_UNASSIGNED);
    h_step(5);
    MT_CHECK(anv_node_id(&h_nodes[0].node) == ANV_NODE_UNASSIGNED);

    assign(0, 21);
    h_step(5);
    MT_CHECK(anv_node_id(&h_nodes[0].node) == 21);
    MT_CHECK(anv_node_state(&h_nodes[0].node) == ANV_NODE_STATE_IDLE);

    /* It announces itself at once with its type and schema hash, then at heartbeat_hz. */
    h_clear_log();
    h_step(1000);
    const int hbs = h_count(ANV_HEARTBEAT_PORT, 21);
    MT_CHECK(hbs >= 9 && hbs <= 11);
    anv_heartbeat_t hb;
    const anv_link_frame_t *f = h_last(ANV_HEARTBEAT_PORT, 21);
    MT_CHECK(f != NULL && anv_heartbeat_unpack(&hb, f->data, f->len));
    MT_CHECK(hb.type_id == ANV_ESP32_BENCH_TYPE_ID);
    MT_CHECK(hb.schema_hash == (uint16_t)ANV_ESP32_BENCH_WIRE_HASH);
    MT_CHECK(hb.state == ANV_NODE_STATE_IDLE);
    MT_CHECK(!hb.leader_seen);
    MT_CHECK(h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED) == 0); /* both stopped asking */

    /* Its filters now pass frames for 21 and for everyone, nothing for other IDs. */
    const int before = h_nodes[0].app.raw_frames;
    const uint8_t x = 1;
    h_send(6, 22, 900, &x, 1);
    h_step(2);
    MT_CHECK(h_nodes[0].app.raw_frames == before);
    h_send(6, 21, 900, &x, 1);
    h_step(2);
    MT_CHECK(h_nodes[0].app.raw_frames == before + 1);
}

static void test_assigned_id_not_kept_by_default(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type);
    MT_CHECK(h_start(0));
    h_step(700);
    assign(0, 30);
    h_step(5);
    MT_CHECK(anv_node_id(&h_nodes[0].node) == 30);
    MT_CHECK(h_start(0)); /* reboot */
    MT_CHECK(anv_node_id(&h_nodes[0].node) == ANV_NODE_UNASSIGNED);
}

static void test_assigned_id_kept(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type)->save_assigned_id = true;
    MT_CHECK(h_start(0));
    h_step(700);
    assign(0, 31);
    h_step(5);
    MT_CHECK(h_start(0)); /* reboot */
    MT_CHECK(anv_node_id(&h_nodes[0].node) == 31);
    MT_CHECK(anv_node_state(&h_nodes[0].node) == ANV_NODE_STATE_IDLE);
    h_clear_log();
    h_step(700);
    MT_CHECK(h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED) == 0);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, 31) >= 1);
}

static void test_saved_fixed_id_and_forget(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type);
    MT_CHECK(h_start(0));
    anv_node_t *n = &h_nodes[0].node;
    MT_CHECK(!anv_node_save_id(n, 0));
    MT_CHECK(!anv_node_save_id(n, ANV_NODE_UNASSIGNED));
    MT_CHECK(anv_node_save_id(n, 40));
    MT_CHECK(anv_node_id(n) == 40);
    MT_CHECK(h_start(0)); /* reboot: still 40 */
    MT_CHECK(anv_node_id(n) == 40);

    /* NMT forget_id from the leader, while idle: the board asks again, and forgets for good. */
    h_step_with_leader(100, 3);
    h_nmt(40, ANV_NMT_COMMAND_FORGET_ID, 3);
    h_step(2);
    MT_CHECK(anv_node_id(n) == ANV_NODE_UNASSIGNED);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_UNASSIGNED);
    h_clear_log();
    h_step(700);
    MT_CHECK(h_count(ANV_ID_REQUEST_PORT, ANV_NODE_UNASSIGNED) >= 1);
    MT_CHECK(h_start(0));
    MT_CHECK(anv_node_id(n) == ANV_NODE_UNASSIGNED);
}

static void test_config_fixed_id(void)
{
    h_reset();
    h_config(0, &anv_esp32_bench_type)->fixed_id = 12;
    MT_CHECK(h_start(0));
    MT_CHECK(anv_node_id(&h_nodes[0].node) == 12);
    MT_CHECK(!anv_node_forget_id(&h_nodes[0].node)); /* a build-time ID can't be forgotten */
    h_step(1);
    MT_CHECK(h_count(ANV_HEARTBEAT_PORT, 12) == 1);

    h_config(1, &anv_esp32_bench_type)->fixed_id = 255;
    MT_CHECK(!h_start(1)); /* not an ID a node may have */
}

int main(void)
{
    MT_RUN(test_asks_until_answered);
    MT_RUN(test_assignment);
    MT_RUN(test_assigned_id_not_kept_by_default);
    MT_RUN(test_assigned_id_kept);
    MT_RUN(test_saved_fixed_id_and_forget);
    MT_RUN(test_config_fixed_id);
    return mt_summary();
}
