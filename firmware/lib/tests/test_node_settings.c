/* anv_node: settings over SERVICE (get, set, limits, save, restore), stream rates, info pages,
 * actions and the label. Uses the example motor_controller type. */
#include <string.h>

#include "anv_motor_controller.h"
#include "minitest.h"
#include "node_harness.h"

#define NODE 10
#define TERM 4
#define KP_0 0x0000u
#define MAX_SPEED 0x0010u
#define COUNTS_PER_REV 0x0020u
#define INVERT_0 0x0021u
#define RUNTIME 0x0040u
#define FEEDBACK 65u
#define THERMAL 67u

static anv_service_t r;

static uint32_t f32(float f)
{
    anv_value_t v;
    v.f = f;
    return v.u;
}

static uint8_t svc(uint8_t op, uint16_t index, uint32_t value)
{
    memset(&r, 0, sizeof r);
    MT_CHECK(h_service(NODE, op, index, value, &r));
    MT_CHECK(r.op == op && r.index == index);
    return r.status;
}

static void boot(void)
{
    h_config(0, &anv_motor_controller_type)->fixed_id = NODE;
    MT_CHECK(h_start(0));
}

static void arm(void)
{
    h_step_with_leader(60, TERM);
    h_nmt(NODE, ANV_NMT_COMMAND_ARM, TERM);
    h_step(1);
    MT_CHECK(anv_node_armed(&h_nodes[0].node));
}

static bool heartbeat_unsaved(void)
{
    h_step(110);
    anv_heartbeat_t hb;
    const anv_link_frame_t *f = h_last(ANV_HEARTBEAT_PORT, NODE);
    return f != NULL && anv_heartbeat_unpack(&hb, f->data, f->len) && hb.unsaved_params;
}

static void test_get(void)
{
    h_reset();
    boot();
    MT_CHECK(svc(ANV_SERVICE_OP_GET, KP_0, 0) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(r.value == f32(10.0f));
    MT_CHECK(svc(ANV_SERVICE_OP_GET, ANV_PARAM_HEARTBEAT_HZ, 0) == ANV_SERVICE_STATUS_OK && r.value == 10u);
    MT_CHECK(svc(ANV_SERVICE_OP_GET, ANV_PARAM_COMMAND_TIMEOUT_MS, 0) == ANV_SERVICE_STATUS_OK && r.value == 100u);
    MT_CHECK(svc(ANV_SERVICE_OP_GET, 0x0E00u, 0) == ANV_SERVICE_STATUS_OK); /* from the included fragment */
    MT_CHECK(svc(ANV_SERVICE_OP_GET, 0x0999u, 0) == ANV_SERVICE_STATUS_BAD_INDEX);
    MT_CHECK(svc(15, 0, 0) == ANV_SERVICE_STATUS_BAD_OP);
}

static void test_set_checks(void)
{
    h_reset();
    boot();
    h_app_t *app = &h_nodes[0].app;
    MT_CHECK(!heartbeat_unsaved());

    MT_CHECK(svc(ANV_SERVICE_OP_SET, KP_0, f32(2000.0f)) == ANV_SERVICE_STATUS_OUT_OF_RANGE);
    MT_CHECK(r.value == f32(10.0f)); /* the response carries the value it has now */
    MT_CHECK(svc(ANV_SERVICE_OP_SET, KP_0, f32(-1.0f)) == ANV_SERVICE_STATUS_OUT_OF_RANGE);
    MT_CHECK(svc(ANV_SERVICE_OP_SET, KP_0, 0x7FC00000u) == ANV_SERVICE_STATUS_OUT_OF_RANGE); /* NaN */
    MT_CHECK(svc(ANV_SERVICE_OP_SET, INVERT_0, 2u) == ANV_SERVICE_STATUS_OUT_OF_RANGE);   /* bool */
    MT_CHECK(svc(ANV_SERVICE_OP_SET, COUNTS_PER_REV, 0u) == ANV_SERVICE_STATUS_OUT_OF_RANGE);
    MT_CHECK(svc(ANV_SERVICE_OP_SET, ANV_PARAM_HEARTBEAT_HZ, 300u) == ANV_SERVICE_STATUS_OUT_OF_RANGE);
    MT_CHECK(svc(ANV_SERVICE_OP_SET, RUNTIME, 5u) == ANV_SERVICE_STATUS_READ_ONLY);
    MT_CHECK(svc(ANV_SERVICE_OP_SET, 0x0999u, 5u) == ANV_SERVICE_STATUS_BAD_INDEX);
    MT_CHECK(app->setting_changes == 0);
    MT_CHECK(!heartbeat_unsaved());

    MT_CHECK(svc(ANV_SERVICE_OP_SET, KP_0, f32(12.5f)) == ANV_SERVICE_STATUS_OK && r.value == f32(12.5f));
    MT_CHECK(app->setting_changes == 1 && app->last_setting == KP_0);
    anv_value_t v;
    MT_CHECK(anv_node_get(&h_nodes[0].node, KP_0, &v) && v.f == 12.5f);
    MT_CHECK(heartbeat_unsaved());
    MT_CHECK(svc(ANV_SERVICE_OP_SET, KP_0, f32(12.5f)) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(app->setting_changes == 1); /* same value: no change reported */

    /* The node itself reports read-only values: the application may write them. */
    v.u = 77;
    MT_CHECK(anv_node_set(&h_nodes[0].node, RUNTIME, v) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(svc(ANV_SERVICE_OP_GET, RUNTIME, 0) == ANV_SERVICE_STATUS_OK && r.value == 77u);
}

static void test_disarmed_only(void)
{
    h_reset();
    boot();
    arm();
    MT_CHECK(svc(ANV_SERVICE_OP_SET, KP_0, f32(5.0f)) == ANV_SERVICE_STATUS_NOT_WHILE_ARMED);
    MT_CHECK(svc(ANV_SERVICE_OP_SET, ANV_PARAM_COMMAND_TIMEOUT_MS, 50u) == ANV_SERVICE_STATUS_NOT_WHILE_ARMED);
    MT_CHECK(svc(ANV_SERVICE_OP_RESTORE, 0, 0) == ANV_SERVICE_STATUS_NOT_WHILE_ARMED);
    MT_CHECK(svc(ANV_SERVICE_OP_SET, MAX_SPEED, f32(20.0f)) == ANV_SERVICE_STATUS_OK); /* not safety */
    MT_CHECK(anv_node_armed(&h_nodes[0].node)); /* a SET never disarms */
}

static void test_save_and_reboot(void)
{
    h_reset();
    boot();
    MT_CHECK(svc(ANV_SERVICE_OP_SET, KP_0, f32(3.25f)) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(svc(ANV_SERVICE_OP_SET, ANV_PARAM_HEARTBEAT_HZ, 20u) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(svc(ANV_SERVICE_OP_SET, ANV_PARAM_DEBUG_LEVEL, 2u) == ANV_SERVICE_STATUS_OK); /* persist: false */
    MT_CHECK(svc(ANV_SERVICE_OP_SET_RATE, THERMAL, 5000u) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(svc(ANV_SERVICE_OP_LABEL_SET, 0, 0x6C656877u) == ANV_SERVICE_STATUS_OK); /* "whel" */
    MT_CHECK(svc(ANV_SERVICE_OP_SAVE, 0, 0) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(!heartbeat_unsaved());

    MT_CHECK(h_start(0)); /* reboot */
    anv_node_t *n = &h_nodes[0].node;
    anv_value_t v;
    MT_CHECK(anv_node_get(n, KP_0, &v) && v.f == 3.25f);
    MT_CHECK(anv_node_get(n, ANV_PARAM_HEARTBEAT_HZ, &v) && v.u == 20u);
    MT_CHECK(anv_node_get(n, ANV_PARAM_DEBUG_LEVEL, &v) && v.u == 0u);
    MT_CHECK(anv_node_stream_rate(n, THERMAL) == 5000u);
    MT_CHECK(memcmp(anv_node_label(n), "whel", 4) == 0);
    MT_CHECK(svc(ANV_SERVICE_OP_LABEL_GET, 0, 0) == ANV_SERVICE_STATUS_OK && r.value == 0x6C656877u);
    MT_CHECK(svc(ANV_SERVICE_OP_LABEL_GET, 4, 0) == ANV_SERVICE_STATUS_BAD_INDEX);
    MT_CHECK(!heartbeat_unsaved());

    /* RESTORE: defaults now, saved values untouched until SAVE. */
    MT_CHECK(svc(ANV_SERVICE_OP_RESTORE, 0, 0) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(anv_node_get(n, KP_0, &v) && v.f == 10.0f);
    MT_CHECK(anv_node_stream_rate(n, THERMAL) == 1000u);
    MT_CHECK(heartbeat_unsaved());
    MT_CHECK(h_start(0));
    MT_CHECK(anv_node_get(n, KP_0, &v) && v.f == 3.25f);
}

static void test_storage_problems(void)
{
    h_reset();
    boot();
    h_nodes[0].store.fail_writes = true;
    MT_CHECK(svc(ANV_SERVICE_OP_SET, KP_0, f32(4.0f)) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(svc(ANV_SERVICE_OP_SAVE, 0, 0) == ANV_SERVICE_STATUS_STORAGE_ERROR);
    MT_CHECK(heartbeat_unsaved());

    /* A damaged saved copy is ignored: defaults. */
    h_nodes[0].store.fail_writes = false;
    MT_CHECK(svc(ANV_SERVICE_OP_SAVE, 0, 0) == ANV_SERVICE_STATUS_OK);
    h_nodes[0].store.slots[0].data[30] ^= 0x40u;
    MT_CHECK(h_start(0));
    anv_value_t v;
    MT_CHECK(anv_node_get(&h_nodes[0].node, KP_0, &v) && v.f == 10.0f);

    /* No storage at all. */
    h_config(1, &anv_motor_controller_type)->fixed_id = 11;
    h_config(1, &anv_motor_controller_type)->storage = NULL;
    MT_CHECK(h_start(1));
    MT_CHECK(anv_node_save(&h_nodes[1].node) == ANV_SERVICE_STATUS_STORAGE_ERROR);
}

static int stream_frames(uint16_t port, uint32_t ms)
{
    h_clear_log();
    h_step(ms);
    return h_count(port, NODE);
}

static void test_stream_rates(void)
{
    h_reset();
    boot();
    MT_CHECK(svc(ANV_SERVICE_OP_GET_RATE, FEEDBACK, 0) == ANV_SERVICE_STATUS_OK && r.value == 50000u);
    int got = stream_frames(FEEDBACK, 1000);
    MT_CHECK(got >= 49 && got <= 51);
    const anv_link_frame_t *f = h_last(FEEDBACK, NODE);
    MT_CHECK(f != NULL && f->len == 7u && f->data[0] == FEEDBACK); /* the schema's length, our data */
    MT_CHECK(anv_id_priority(f->id) == ANV_MOTOR_CONTROLLER_FEEDBACK_PRIORITY);

    MT_CHECK(svc(ANV_SERVICE_OP_SET_RATE, FEEDBACK, 10000u) == ANV_SERVICE_STATUS_OK && r.value == 10000u);
    got = stream_frames(FEEDBACK, 1000);
    MT_CHECK(got >= 9 && got <= 11);
    MT_CHECK(svc(ANV_SERVICE_OP_SET_RATE, FEEDBACK, 2500u) == ANV_SERVICE_STATUS_OK); /* 2.5 Hz */
    got = stream_frames(FEEDBACK, 2000);
    MT_CHECK(got == 5);
    MT_CHECK(svc(ANV_SERVICE_OP_SET_RATE, FEEDBACK, 0u) == ANV_SERVICE_STATUS_OK); /* off */
    MT_CHECK(stream_frames(FEEDBACK, 1000) == 0);

    MT_CHECK(svc(ANV_SERVICE_OP_SET_RATE, FEEDBACK, 300000u) == ANV_SERVICE_STATUS_OUT_OF_RANGE);
    MT_CHECK(r.value == 0u);
    MT_CHECK(svc(ANV_SERVICE_OP_SET_RATE, 64u, 1000u) == ANV_SERVICE_STATUS_BAD_INDEX); /* a command */
    MT_CHECK(svc(ANV_SERVICE_OP_GET_RATE, 900u, 0) == ANV_SERVICE_STATUS_BAD_INDEX);

    /* Other streams kept their own rates; the fragment's stream too. */
    got = stream_frames(THERMAL, 2000);
    MT_CHECK(got == 2);
    got = stream_frames(200u, 1000); /* power, 5 Hz */
    MT_CHECK(got >= 4 && got <= 6);

    /* The heartbeat follows its setting. */
    MT_CHECK(svc(ANV_SERVICE_OP_SET, ANV_PARAM_HEARTBEAT_HZ, 50u) == ANV_SERVICE_STATUS_OK);
    got = stream_frames(ANV_HEARTBEAT_PORT, 1000);
    MT_CHECK(got >= 49 && got <= 51);
}

static void test_info_and_actions(void)
{
    h_reset();
    boot();
    h_app_t *app = &h_nodes[0].app;
    MT_CHECK(svc(ANV_SERVICE_OP_INFO, ANV_INFO_PAGE_TYPE_ID, 0) == ANV_SERVICE_STATUS_OK && r.value == 100u);
    MT_CHECK(svc(ANV_SERVICE_OP_INFO, ANV_INFO_PAGE_SCHEMA_HASH, 0) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(r.value == ANV_MOTOR_CONTROLLER_WIRE_HASH);
    MT_CHECK(svc(ANV_SERVICE_OP_INFO, ANV_INFO_PAGE_FW_BUILD, 0) == ANV_SERVICE_STATUS_OK && r.value == 0x1234u);
    MT_CHECK(svc(ANV_SERVICE_OP_INFO, ANV_INFO_PAGE_MAC_LOW, 0) == ANV_SERVICE_STATUS_OK && r.value == 1u);
    MT_CHECK(svc(ANV_SERVICE_OP_INFO, ANV_INFO_PAGE_MAC_HIGH, 0) == ANV_SERVICE_STATUS_OK && r.value == 0x0200u);
    MT_CHECK(svc(ANV_SERVICE_OP_INFO, ANV_INFO_PAGE_RX_FRAMES, 0) == ANV_SERVICE_STATUS_OK && r.value >= 1u);
    MT_CHECK(svc(ANV_SERVICE_OP_INFO, 99, 0) == ANV_SERVICE_STATUS_BAD_INDEX);

    MT_CHECK(svc(ANV_SERVICE_OP_ACTION, ANV_MOTOR_CONTROLLER_ACTION_ZERO_COUNTS, 0) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(app->actions == 1 && app->last_action == ANV_MOTOR_CONTROLLER_ACTION_ZERO_COUNTS);
    MT_CHECK(svc(ANV_SERVICE_OP_ACTION, ANV_MOTOR_CONTROLLER_ACTION_AUTOTUNE, 300u) ==
             ANV_SERVICE_STATUS_OUT_OF_RANGE); /* u8 argument */
    MT_CHECK(svc(ANV_SERVICE_OP_ACTION, ANV_MOTOR_CONTROLLER_ACTION_AUTOTUNE, 1u) == ANV_SERVICE_STATUS_OK);
    MT_CHECK(app->last_action_arg == 1u);
    MT_CHECK(svc(ANV_SERVICE_OP_ACTION, 9, 0) == ANV_SERVICE_STATUS_BAD_INDEX);
    arm();
    MT_CHECK(svc(ANV_SERVICE_OP_ACTION, ANV_MOTOR_CONTROLLER_ACTION_JOG, f32(1.0f)) ==
             ANV_SERVICE_STATUS_NOT_WHILE_ARMED);
    MT_CHECK(app->actions == 2);
}

static void test_requests_for_others_ignored(void)
{
    h_reset();
    boot();
    anv_service_t resp;
    MT_CHECK(!h_service(NODE + 1, ANV_SERVICE_OP_GET, KP_0, 0, &resp));
    MT_CHECK(h_nodes[0].node.stats.services == 0);
}

int main(void)
{
    MT_RUN(test_get);
    MT_RUN(test_set_checks);
    MT_RUN(test_disarmed_only);
    MT_RUN(test_save_and_reboot);
    MT_RUN(test_storage_problems);
    MT_RUN(test_stream_rates);
    MT_RUN(test_info_and_actions);
    MT_RUN(test_requests_for_others_ignored);
    return mt_summary();
}
