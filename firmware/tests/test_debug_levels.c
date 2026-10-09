/* Debug levels: what each level changes, DEBUG_LEVEL handling (target, master, mask), the level
 * reported in NODE_HB, and the auto-off timer. */
#include "minitest.h"
#include "test_diag_helpers.h"

#define NODE ROVER_NODE_DRIVE1
#define MASTER ROVER_NODE_ORIN
#define TERM 5u

static void test_levels_decide_which_events_reach_the_bus(void)
{
    anv_debug_t d;
    anv_debug_init(&d, 0);
    MT_CHECK(!anv_debug_event_allowed(&d, ROVER_SEVERITY_INFO));
    MT_CHECK(!anv_debug_event_allowed(&d, ROVER_SEVERITY_WARNING));
    MT_CHECK(anv_debug_event_allowed(&d, ROVER_SEVERITY_ERROR));      /* errors and criticals: always */
    MT_CHECK(anv_debug_event_allowed(&d, ROVER_SEVERITY_CRITICAL));

    anv_debug_init(&d, 1);
    MT_CHECK(!anv_debug_event_allowed(&d, ROVER_SEVERITY_INFO));
    MT_CHECK(anv_debug_event_allowed(&d, ROVER_SEVERITY_WARNING));

    anv_debug_init(&d, 2);
    MT_CHECK(anv_debug_event_allowed(&d, ROVER_SEVERITY_INFO));
    anv_debug_init(&d, 3);
    MT_CHECK(anv_debug_event_allowed(&d, ROVER_SEVERITY_INFO));
}

static void test_levels_decide_telemetry_and_debug_page_rates(void)
{
    anv_debug_t d;
    const uint32_t expect_pages[] = {0u, 0u, 10u, 50u};
    for (uint8_t level = 0; level <= 3; level++) {
        anv_debug_init(&d, level);
        MT_CHECK(anv_debug_page_rate_hz(&d) == expect_pages[level]);
        /* Level 0 slows telemetry to 1 Hz; every other level uses the parameter rate. */
        MT_CHECK(anv_debug_telemetry_rate_hz(&d, 20u) == (level == 0u ? 1u : 20u));
    }
}

static void test_category_mask_limits_what_a_level_applies_to(void)
{
    anv_debug_t d;
    anv_debug_init(&d, 1);

    /* "Debug pages only": level 2 on the pages category, everything else stays at baseline. */
    MT_CHECK(anv_debug_apply(&d, 2, ANV_DEBUG_CAT_PAGES, 0, 0));
    MT_CHECK(anv_debug_page_rate_hz(&d) == 10u);
    MT_CHECK(!anv_debug_event_allowed(&d, ROVER_SEVERITY_INFO));
    MT_CHECK(anv_debug_telemetry_rate_hz(&d, 20u) == 20u);

    /* "Events only" at trace: info events on, no debug pages. */
    MT_CHECK(anv_debug_apply(&d, 3, ANV_DEBUG_CAT_EVENTS, 0, 0));
    MT_CHECK(anv_debug_event_allowed(&d, ROVER_SEVERITY_INFO));
    MT_CHECK(anv_debug_page_rate_hz(&d) == 0u);

    /* Level 0 for telemetry only: 1 Hz telemetry, events untouched. */
    MT_CHECK(anv_debug_apply(&d, 0, ANV_DEBUG_CAT_TELEMETRY, 0, 0));
    MT_CHECK(anv_debug_telemetry_rate_hz(&d, 20u) == 1u);
    MT_CHECK(anv_debug_event_allowed(&d, ROVER_SEVERITY_WARNING));
}

static void test_level_above_3_is_refused(void)
{
    anv_debug_t d;
    anv_debug_init(&d, 1);
    MT_CHECK(!anv_debug_apply(&d, 4, ANV_DEBUG_CAT_ALL, 0, 0));
    MT_CHECK(anv_debug_level(&d) == 1u);
}

/* Reads the debug_level field out of the next NODE_HB the node sends. */
static int hb_debug_level(test_rig_t *rig, uint32_t t)
{
    (void)test_rig_drain(rig);
    acc_can_frame_t frame;
    while (rig->host_driver.receive(rig->host_driver.ctx, &frame)) {
        if (frame.id == ROVER_CAN_ID(ROVER_CLASS_HEARTBEAT, NODE, 0u)) {
            struct rover_drive1_node_hb_t hb;
            if (rover_drive1_node_hb_unpack(&hb, frame.data, frame.dlc) == 0) {
                return hb.debug_level;
            }
        }
    }
    (void)t;
    return -1;
}

static void test_debug_level_frame_changes_level_and_heartbeat_reports_it(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    anv_node_set_hb_rate(&rig.node, 10u, t);
    test_rig_become_master_and_arm(&rig, MASTER, TERM, 1u, t);

    t += 100u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(hb_debug_level(&rig, t) == 1);

    acc_can_frame_t f = make_debug_level_frame(NODE, 3, ANV_DEBUG_CAT_ALL, 0, MASTER, TERM);
    test_rig_host_send(&rig, &f);
    test_rig_pump(&rig, t);
    t += 100u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(anv_debug_level(&rig.node.debug) == 3u);
    MT_CHECK(hb_debug_level(&rig, t) == 3);

    /* Broadcast target (0) reaches every node. */
    f = make_debug_level_frame(0, 0, ANV_DEBUG_CAT_ALL, 0, MASTER, TERM);
    test_rig_host_send(&rig, &f);
    test_rig_pump(&rig, t);
    t += 100u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(hb_debug_level(&rig, t) == 0);
}

static void test_debug_level_frame_ignored_unless_from_master_for_this_node(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    test_rig_become_master_and_arm(&rig, MASTER, TERM, 1u, t);

    acc_can_frame_t other_node = make_debug_level_frame(ROVER_NODE_DRIVE2, 3, ANV_DEBUG_CAT_ALL, 0, MASTER, TERM);
    test_rig_host_send(&rig, &other_node);
    acc_can_frame_t not_master = make_debug_level_frame(NODE, 3, ANV_DEBUG_CAT_ALL, 0, ROVER_NODE_RPI, TERM);
    test_rig_host_send(&rig, &not_master);
    acc_can_frame_t old_term = make_debug_level_frame(NODE, 3, ANV_DEBUG_CAT_ALL, 0, MASTER, TERM - 1u);
    test_rig_host_send(&rig, &old_term);
    acc_can_frame_t bad_level = make_debug_level_frame(NODE, 3, ANV_DEBUG_CAT_ALL, 0, MASTER, TERM);
    bad_level.data[1] = 9u; /* level 9 */
    test_rig_host_send(&rig, &bad_level);
    test_rig_pump(&rig, t);

    MT_CHECK(anv_debug_level(&rig.node.debug) == 1u);
}

static void test_level_changes_what_the_node_sends(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 0);
    seen_event_t ev[SEEN_MAX];

    anv_node_event(&rig.node, ROVER_ERR_BAD_FRAME, 1u, t);        /* warning */
    anv_node_event(&rig.node, ROVER_ERR_MASTER_CHANGED, 1u, t);   /* info */
    anv_node_event(&rig.node, ROVER_ERR_HARD_FAULT, 1u, t);       /* critical */
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);

    test_rig_become_master_and_arm(&rig, MASTER, TERM, 1u, t);
    acc_can_frame_t f = make_debug_level_frame(NODE, 1, ANV_DEBUG_CAT_ALL, 0, MASTER, TERM);
    test_rig_host_send(&rig, &f);
    test_rig_pump(&rig, t);
    (void)collect_events(&rig, ev, SEEN_MAX);
    t += 2000u;
    anv_node_event(&rig.node, ROVER_ERR_BAD_FRAME, 2u, t);
    anv_node_event(&rig.node, ROVER_ERR_MASTER_CHANGED, 2u, t);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u && ev[0].code == ROVER_ERR_BAD_FRAME);

    f = make_debug_level_frame(NODE, 2, ANV_DEBUG_CAT_ALL, 0, MASTER, TERM);
    test_rig_host_send(&rig, &f);
    test_rig_pump(&rig, t);
    t += 2000u;
    anv_node_event(&rig.node, ROVER_ERR_BAD_FRAME, 3u, t);
    anv_node_event(&rig.node, ROVER_ERR_MASTER_CHANGED, 3u, t);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 2u);
}

static void test_auto_off_timer_returns_to_default_level(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    test_rig_become_master_and_arm(&rig, MASTER, TERM, 1u, t);

    acc_can_frame_t f = make_debug_level_frame(NODE, 3, ANV_DEBUG_CAT_ALL, 2 /* min */, MASTER, TERM);
    test_rig_host_send(&rig, &f);
    test_rig_pump(&rig, t);
    uint32_t start = t;
    MT_CHECK(anv_debug_level(&rig.node.debug) == 3u);
    MT_CHECK(anv_debug_minutes_left(&rig.node.debug, start) == 2u);

    t = start + 119000u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(anv_debug_level(&rig.node.debug) == 3u);
    MT_CHECK(anv_debug_minutes_left(&rig.node.debug, t) == 1u);

    t = start + 120000u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(anv_debug_level(&rig.node.debug) == 1u); /* back to the debug_default parameter */
    MT_CHECK(rig.node.debug.category_mask == ANV_DEBUG_CAT_ALL);
    MT_CHECK(anv_debug_minutes_left(&rig.node.debug, t) == 0u);
}

static void test_new_request_replaces_or_cancels_the_timer(void)
{
    anv_debug_t d;
    anv_debug_init(&d, 1);

    /* "Until changed" has no timer. */
    anv_debug_apply(&d, 3, ANV_DEBUG_CAT_ALL, 0, 1000u);
    MT_CHECK(!anv_debug_tick(&d, 1000u + 3600u * 1000u * 10u));
    MT_CHECK(anv_debug_level(&d) == 3u);

    /* A new request restarts the timer from now. */
    anv_debug_apply(&d, 2, ANV_DEBUG_CAT_ALL, 1, 0u);
    anv_debug_apply(&d, 3, ANV_DEBUG_CAT_ALL, 1, 30000u);
    MT_CHECK(!anv_debug_tick(&d, 60000u));
    MT_CHECK(anv_debug_tick(&d, 90000u));
    MT_CHECK(anv_debug_level(&d) == 1u);

    /* Asking for the default level cancels a running timer. */
    anv_debug_apply(&d, 3, ANV_DEBUG_CAT_ALL, 1, 0u);
    anv_debug_apply(&d, 1, ANV_DEBUG_CAT_ALL, 1, 10u);
    MT_CHECK(!d.timer_active);
}

static void test_timer_survives_millisecond_counter_wrap(void)
{
    anv_debug_t d;
    anv_debug_init(&d, 1);
    uint32_t start = 0xFFFFFFFFu - 30000u; /* 30 s before the 32-bit ms counter wraps */
    anv_debug_apply(&d, 3, ANV_DEBUG_CAT_ALL, 1, start);
    MT_CHECK(!anv_debug_tick(&d, start + 10000u));
    MT_CHECK(anv_debug_tick(&d, start + 60000u)); /* wrapped past zero */
}

static void test_reboot_returns_to_the_default_level(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    diag_rig_boot(&rig, NODE, 1000u, 2);
    MT_CHECK(anv_debug_level(&rig.node.debug) == 2u);
    test_rig_become_master_and_arm(&rig, MASTER, TERM, 1u, 1000u);
    acc_can_frame_t f = make_debug_level_frame(NODE, 3, ANV_DEBUG_CAT_ALL, 0, MASTER, TERM);
    test_rig_host_send(&rig, &f);
    test_rig_pump(&rig, 1000u);
    MT_CHECK(anv_debug_level(&rig.node.debug) == 3u);

    diag_rig_boot(&rig, NODE, 1000u, 2);
    MT_CHECK(anv_debug_level(&rig.node.debug) == 2u);
}

int main(void)
{
    MT_RUN(test_levels_decide_which_events_reach_the_bus);
    MT_RUN(test_levels_decide_telemetry_and_debug_page_rates);
    MT_RUN(test_category_mask_limits_what_a_level_applies_to);
    MT_RUN(test_level_above_3_is_refused);
    MT_RUN(test_debug_level_frame_changes_level_and_heartbeat_reports_it);
    MT_RUN(test_debug_level_frame_ignored_unless_from_master_for_this_node);
    MT_RUN(test_level_changes_what_the_node_sends);
    MT_RUN(test_auto_off_timer_returns_to_default_level);
    MT_RUN(test_new_request_replaces_or_cancels_the_timer);
    MT_RUN(test_timer_survives_millisecond_counter_wrap);
    MT_RUN(test_reboot_returns_to_the_default_level);
    return mt_summary();
}
