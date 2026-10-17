/* anv_node: arming, command delivery and its watchdog, E-stop, leader tracking, faults. These
 * can't be switched off by the application, so every test goes through the bus as a host would.
 * Uses the example motor_controller type (setpoint: port 64, watchdog 100 ms, applied at SYNC). */
#include <string.h>

#include "anv_motor_controller.h"
#include "minitest.h"
#include "node_harness.h"

#define NODE 10
#define TERM 4
#define SETPOINT ANV_MOTOR_CONTROLLER_SETPOINT_PORT

static anv_node_t *n;
static h_app_t *app;

static void boot(void)
{
    h_reset();
    h_config(0, &anv_motor_controller_type)->fixed_id = NODE;
    MT_CHECK(h_start(0));
    n = &h_nodes[0].node;
    app = &h_nodes[0].app;
}

static void setpoint_from(anv_node_id_t src, uint8_t marker)
{
    uint8_t d[8] = {marker, 0, 0, 0, 0, 0, 0, 0};
    anv_link_frame_t f;
    const anv_proto_addr_t a = {ANV_MOTOR_CONTROLLER_SETPOINT_PRIORITY, NODE, SETPOINT, src};
    anv_proto_frame(&f, &a, d, ANV_MOTOR_CONTROLLER_SETPOINT_LENGTH);
    anv_loopback_inject(&h_bus, &f);
}

static void sync_from(anv_node_id_t src)
{
    const anv_sync_t s = {1, 20};
    uint8_t d[8];
    const uint8_t len = anv_sync_pack(d, &s);
    anv_link_frame_t f;
    const anv_proto_addr_t a = {ANV_SYNC_PRIORITY, ANV_NODE_BROADCAST, ANV_SYNC_PORT, src};
    anv_proto_frame(&f, &a, d, len);
    anv_loopback_inject(&h_bus, &f);
}

static void arm(void)
{
    h_step_with_leader(60, TERM);
    h_nmt(NODE, ANV_NMT_COMMAND_ARM, TERM);
    h_step(1);
}

/* Leader heartbeats plus a setpoint and SYNC every 20 ms, as a leading host does. */
static void drive(uint32_t ms)
{
    for (uint32_t k = 0; k < ms; k++) {
        if (h_now % 20u == 0u) {
            setpoint_from(H_HOST_ID, 1);
            sync_from(H_HOST_ID);
        }
        h_step_with_leader(1, TERM);
    }
}

static void test_outputs_off_until_armed(void)
{
    boot();
    MT_CHECK(app->outputs_off == 1); /* at begin, whatever the hardware came up as */
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE);
    MT_CHECK(!anv_node_armed(n));

    /* No leader: ARM is not accepted from anyone. */
    h_nmt(NODE, ANV_NMT_COMMAND_ARM, 0);
    h_step(1);
    MT_CHECK(!anv_node_armed(n));

    /* A leader, but the wrong term or another host: still not. */
    h_step_with_leader(60, TERM);
    MT_CHECK(anv_node_has_leader(n) && anv_node_leader(n) == H_HOST_ID);
    h_nmt(NODE, ANV_NMT_COMMAND_ARM, TERM + 1);
    h_step(1);
    MT_CHECK(!anv_node_armed(n));
    const anv_nmt_t m = {ANV_NMT_COMMAND_ARM, TERM};
    uint8_t d[8];
    anv_link_frame_t f;
    const anv_proto_addr_t a = {ANV_NMT_PRIORITY, NODE, ANV_NMT_PORT, 2};
    anv_proto_frame(&f, &a, d, anv_nmt_pack(d, &m));
    anv_loopback_inject(&h_bus, &f);
    h_step(1);
    MT_CHECK(!anv_node_armed(n));

    h_nmt(NODE, ANV_NMT_COMMAND_ARM, TERM);
    h_step(1);
    MT_CHECK(anv_node_armed(n));
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_ARMED);

    h_nmt(ANV_NODE_BROADCAST, ANV_NMT_COMMAND_DISARM, TERM); /* to every node */
    h_step(1);
    MT_CHECK(!anv_node_armed(n));
    MT_CHECK(app->outputs_off == 2);
}

static void test_commands(void)
{
    boot();
    setpoint_from(H_HOST_ID, 1);
    h_step(1);
    MT_CHECK(app->commands == 0); /* disarmed: not delivered */
    MT_CHECK(n->stats.commands_ignored == 1);

    arm();
    setpoint_from(H_HOST_ID, 2);
    h_step(5);
    MT_CHECK(app->commands == 0); /* apply: sync, so held */
    setpoint_from(H_HOST_ID, 3);  /* the latest wins */
    sync_from(2);                 /* SYNC from a host that doesn't lead: no */
    h_step(1);
    MT_CHECK(app->commands == 0 && app->syncs == 0);
    sync_from(H_HOST_ID);
    h_step(1);
    MT_CHECK(app->commands == 1 && app->syncs == 1);
    MT_CHECK(app->last_command_port == SETPOINT && app->last_command[0] == 3);
    sync_from(H_HOST_ID);
    h_step(1);
    MT_CHECK(app->commands == 1); /* delivered once */

    setpoint_from(2, 4); /* not the leader */
    sync_from(H_HOST_ID);
    h_step(1);
    MT_CHECK(app->commands == 1);

    /* Too short for the schema's layout. */
    anv_link_frame_t f;
    const anv_proto_addr_t a = {1, NODE, SETPOINT, H_HOST_ID};
    const uint8_t d[3] = {9, 9, 9};
    anv_proto_frame(&f, &a, d, 3);
    anv_loopback_inject(&h_bus, &f);
    sync_from(H_HOST_ID);
    h_step(1);
    MT_CHECK(app->commands == 1);

    /* Another node's broadcast on the same port number is its own message, not a command. */
    const int raw = app->raw_frames;
    const anv_proto_addr_t other = {2, ANV_NODE_BROADCAST, SETPOINT, 33};
    anv_proto_frame(&f, &other, d, 3);
    anv_loopback_inject(&h_bus, &f);
    h_step(1);
    MT_CHECK(app->raw_frames == raw + 1);
}

static void test_watchdog(void)
{
    boot();
    arm();
    drive(500);
    MT_CHECK(anv_node_armed(n));
    MT_CHECK(app->commands >= 24);

    /* The leader keeps its heartbeat but stops commanding: disarmed within the watchdog. */
    const uint32_t last = h_now - (h_now % 20u);
    while (anv_node_armed(n) && h_now - last < 300u) {
        h_step_with_leader(1, TERM);
    }
    MT_CHECK(!anv_node_armed(n));
    MT_CHECK(h_now - last >= 99u && h_now - last <= 101u);
    MT_CHECK(n->stats.watchdog_trips == 1);
    MT_CHECK(anv_node_has_leader(n)); /* only the node disarmed */

    /* A shorter command_timeout_ms wins over the schema's 100 ms. */
    anv_value_t v;
    v.u = 40;
    MT_CHECK(anv_node_set(n, ANV_PARAM_COMMAND_TIMEOUT_MS, v) == ANV_SERVICE_STATUS_OK);
    arm();
    drive(100);
    const uint32_t last2 = h_now - (h_now % 20u);
    while (anv_node_armed(n) && h_now - last2 < 300u) {
        h_step_with_leader(1, TERM);
    }
    MT_CHECK(h_now - last2 >= 39u && h_now - last2 <= 41u);

    /* Arming starts the clock: no command at all after ARM disarms too. */
    arm();
    MT_CHECK(anv_node_armed(n));
    h_step_with_leader(45, TERM);
    MT_CHECK(!anv_node_armed(n));
}

static void test_estop(void)
{
    boot();
    arm();
    drive(100);
    const int off = app->outputs_off;
    h_estop(77); /* anyone may send it, from any ID */
    h_step(1);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_ESTOP);
    MT_CHECK(!anv_node_armed(n) && app->outputs_off == off + 1);
    MT_CHECK(n->stats.estops == 1);

    /* Latched: nothing arms it, commands do nothing. */
    h_nmt(NODE, ANV_NMT_COMMAND_ARM, TERM);
    drive(50);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_ESTOP);

    /* A bad magic byte is not an E-stop. */
    boot();
    const uint8_t bad[2] = {0x00, 0};
    h_send(0, ANV_NODE_BROADCAST, ANV_ESTOP_PORT, bad, 2);
    h_step(1);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE);

    /* Release: only from the leader, with its term. */
    h_estop(77);
    h_step_with_leader(60, TERM);
    anv_estop_release_t rel = {TERM + 1};
    uint8_t d[8];
    h_send(0, ANV_NODE_BROADCAST, ANV_ESTOP_RELEASE_PORT, d, anv_estop_release_pack(d, &rel));
    h_step(1);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_ESTOP);
    rel.term = TERM;
    h_send(0, ANV_NODE_BROADCAST, ANV_ESTOP_RELEASE_PORT, d, anv_estop_release_pack(d, &rel));
    h_step(1);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE); /* released to idle, never straight to armed */

    /* The node's own E-stop input: latches here and tells everyone. */
    h_clear_log();
    anv_node_estop(n, 3);
    h_step(1);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_ESTOP);
    anv_estop_t e;
    const anv_link_frame_t *f = h_last(ANV_ESTOP_PORT, NODE);
    MT_CHECK(f != NULL && anv_estop_unpack(&e, f->data, f->len) && e.reason == 3);
    MT_CHECK(anv_id_priority(f->id) == 0u);

    /* A board without an ID still latches, and stays latched once it gets one. */
    h_reset();
    h_config(0, &anv_motor_controller_type);
    MT_CHECK(h_start(0));
    h_estop(77);
    h_step(1);
    MT_CHECK(h_nodes[0].node.estop_latched);
    MT_CHECK(anv_node_save_id(&h_nodes[0].node, 12));
    MT_CHECK(anv_node_state(&h_nodes[0].node) == ANV_NODE_STATE_ESTOP);
}

static void test_leader_change(void)
{
    boot();
    arm();
    drive(100);
    MT_CHECK(anv_node_armed(n));

    /* An older term, or the same term from another host, changes nothing. */
    h_host_heartbeat(2, ANV_HOST_ROLE_LEADER, TERM - 1);
    h_host_heartbeat(2, ANV_HOST_ROLE_LEADER, TERM);
    drive(20);
    MT_CHECK(anv_node_armed(n) && anv_node_leader(n) == H_HOST_ID);
    MT_CHECK(n->stats.leader_changes == 0);

    /* A higher term takes over: disarm, and the old leader is ignored from now on. */
    const int off = app->outputs_off;
    h_host_heartbeat(2, ANV_HOST_ROLE_LEADER, TERM + 1);
    h_step(1);
    MT_CHECK(!anv_node_armed(n));
    MT_CHECK(app->outputs_off == off + 1);
    MT_CHECK(anv_node_leader(n) == 2 && n->stats.leader_changes == 1);
    h_nmt(NODE, ANV_NMT_COMMAND_ARM, TERM);
    h_step(1);
    MT_CHECK(!anv_node_armed(n));

    /* The same host claiming again with a new term is a new leadership too. */
    boot();
    arm();
    drive(40);
    h_host_heartbeat(H_HOST_ID, ANV_HOST_ROLE_LEADER, TERM + 1);
    h_step(1);
    MT_CHECK(!anv_node_armed(n));

    /* The leader stepping down. */
    boot();
    arm();
    drive(40);
    h_host_heartbeat(H_HOST_ID, ANV_HOST_ROLE_STANDBY, TERM);
    h_step(1);
    MT_CHECK(!anv_node_armed(n) && !anv_node_has_leader(n));
}

static void test_leader_lost(void)
{
    boot();
    h_step_with_leader(100, TERM);
    MT_CHECK(anv_node_has_leader(n));

    /* Commands keep coming but the leader's heartbeat stops (leader_timeout_ms = 250). */
    arm();
    for (uint32_t k = 0; k < 400 && anv_node_has_leader(n); k++) {
        if (h_now % 20u == 0u) {
            setpoint_from(H_HOST_ID, 1);
            sync_from(H_HOST_ID);
        }
        h_step(1);
    }
    MT_CHECK(!anv_node_has_leader(n) && !anv_node_armed(n));
    MT_CHECK(n->stats.leader_losses == 1);
    h_step(110);
    anv_heartbeat_t hb;
    const anv_link_frame_t *f = h_last(ANV_HEARTBEAT_PORT, NODE);
    MT_CHECK(f != NULL && anv_heartbeat_unpack(&hb, f->data, f->len) && !hb.leader_seen);

    /* Once lost, a restarted host may lead again with a fresh (lower) term. */
    h_step_with_leader(60, 1);
    MT_CHECK(anv_node_has_leader(n));
}

static void test_fault(void)
{
    boot();
    arm();
    drive(40);
    anv_node_set_fault(n, true);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_FAULT && !anv_node_armed(n));
    h_nmt(NODE, ANV_NMT_COMMAND_FAULT_RESET, TERM);
    h_step_with_leader(1, TERM);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_FAULT); /* still active */
    anv_node_set_fault(n, false);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_FAULT); /* latched until reset */
    h_nmt(NODE, ANV_NMT_COMMAND_ARM, TERM);
    h_step_with_leader(1, TERM);
    MT_CHECK(!anv_node_armed(n));
    h_nmt(NODE, ANV_NMT_COMMAND_FAULT_RESET, TERM);
    h_step_with_leader(1, TERM);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE);

    /* E-stop beats fault: both latched, E-stop shows; releasing it leaves the fault. */
    anv_node_set_fault(n, true);
    h_estop(77);
    h_step_with_leader(1, TERM);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_ESTOP);
    const anv_estop_release_t rel = {TERM};
    uint8_t d[8];
    h_send(0, ANV_NODE_BROADCAST, ANV_ESTOP_RELEASE_PORT, d, anv_estop_release_pack(d, &rel));
    h_step_with_leader(1, TERM);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_FAULT);
}

static void test_hooks(void)
{
    boot();
    MT_CHECK(app->setups == 1);
    h_step(10);
    MT_CHECK(app->loops == 10);
    h_step_with_leader(60, TERM);
    h_nmt(NODE, ANV_NMT_COMMAND_IDENTIFY, TERM);
    h_step(1);
    MT_CHECK(app->identifies == 1);
    arm();
    h_nmt(NODE, ANV_NMT_COMMAND_RESET, TERM);
    h_step(1);
    MT_CHECK(app->resets == 1 && !anv_node_armed(n)); /* outputs off before the reset hook */

    /* Raw frames: in through on_frame, out with this node's ID as the source. */
    const uint8_t x[2] = {1, 2};
    h_send(6, NODE, 900, x, 2);
    h_step(1);
    MT_CHECK(app->raw_frames >= 1);
    anv_link_frame_t f;
    const anv_proto_addr_t a = {6, ANV_NODE_BROADCAST, 901, 99};
    anv_proto_frame(&f, &a, x, 2);
    h_clear_log();
    MT_CHECK(anv_node_send_frame(n, &f));
    h_step(1);
    MT_CHECK(h_count(901, NODE) == 1 && h_count(901, 99) == 0);

    /* anv_node_send: the type's own messages only, never a command. */
    MT_CHECK(anv_node_send(n, 65u, x, 2));
    MT_CHECK(!anv_node_send(n, SETPOINT, x, 2));
    MT_CHECK(!anv_node_send(n, 900u, x, 2));

    /* The application can't run without an outputs_off hook. */
    anv_node_config_t *c = h_config(1, &anv_motor_controller_type);
    c->outputs_off = NULL;
    MT_CHECK(!h_start(1));
}

int main(void)
{
    MT_RUN(test_outputs_off_until_armed);
    MT_RUN(test_commands);
    MT_RUN(test_watchdog);
    MT_RUN(test_estop);
    MT_RUN(test_leader_change);
    MT_RUN(test_leader_lost);
    MT_RUN(test_fault);
    MT_RUN(test_hooks);
    return mt_summary();
}
