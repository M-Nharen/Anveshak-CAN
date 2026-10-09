/* Black box: RAM ring, flash log across a reset, boot count, LOG_READ / LOG_CLEAR, crash
 * records, reset causes and the power-on self-test. */
#include "minitest.h"
#include "test_diag_helpers.h"

#define NODE ROVER_NODE_DRIVE1
#define REQ_SUB 0u

typedef struct {
    uint8_t opcode;
    uint8_t status;
    uint8_t txn;
    uint16_t index;
    uint32_t value;
} resp_t;

/* Sends one SERVICE request, runs the node and returns the responses seen by the host. */
static size_t service(test_rig_t *rig, uint8_t opcode, uint16_t index, resp_t *out, size_t max, uint32_t t)
{
    acc_can_frame_t req = make_service_frame(NODE, REQ_SUB, 9u, opcode, index, 0u);
    test_rig_host_send(rig, &req);
    test_rig_pump(rig, t);
    (void)test_rig_drain(rig);

    size_t n = 0;
    acc_can_frame_t frame;
    while (rig->host_driver.receive(rig->host_driver.ctx, &frame)) {
        if (frame.id != ROVER_CAN_ID(ROVER_CLASS_SERVICE, NODE, REQ_SUB + 4u) || n >= max) {
            continue;
        }
        struct rover_drive1_service_resp_orin_t r;
        if (rover_drive1_service_resp_orin_unpack(&r, frame.data, frame.dlc) != 0) {
            continue;
        }
        out[n].opcode = r.opcode;
        out[n].status = r.status;
        out[n].txn = r.transaction_id;
        out[n].index = r.param_index;
        out[n].value = r.value_raw;
        n++;
    }
    return n;
}

static void drain_host(test_rig_t *rig)
{
    seen_event_t ev[SEEN_MAX];
    (void)collect_events(rig, ev, SEEN_MAX);
}

static void test_ram_ring_keeps_the_last_64(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 2);
    for (uint16_t i = 0; i < 70; i++) {
        t += 1000u;
        anv_node_event(&rig.node, ROVER_ERR_MASTER_CHANGED, i, t);
        drain_host(&rig);
    }
    MT_CHECK(anv_diag_log_count(&rig.node.diag) == ANV_LOG_RAM_ENTRIES);
    anv_log_entry_t e;
    MT_CHECK(anv_diag_log_get(&rig.node.diag, 0, &e) && e.detail == 69u); /* newest first */
    MT_CHECK(anv_diag_log_get(&rig.node.diag, 63, &e) && e.detail == 6u);
    MT_CHECK(!anv_diag_log_get(&rig.node.diag, 64, &e));
}

static void test_flash_log_survives_reset_and_keeps_last_32_errors(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 2);
    for (uint16_t i = 0; i < 40; i++) {
        t += 1000u;
        anv_node_event(&rig.node, ROVER_ERR_OVERCURRENT_A, i, t);
        anv_node_event(&rig.node, ROVER_ERR_MASTER_CHANGED, i, t); /* info: RAM only */
        anv_node_tick(&rig.node, t);
        drain_host(&rig);
    }
    t += 1000u;
    anv_node_event(&rig.node, ROVER_ERR_HARD_FAULT, 0xBEEFu, t); /* critical: written to flash at once */

    /* Reset: all RAM gone, only g_hw (flash) is left. */
    diag_rig_boot(&rig, NODE, 1000u, 2);
    MT_CHECK(anv_diag_log_count(&rig.node.diag) == ANV_LOG_FLASH_ENTRIES);
    anv_log_entry_t e;
    MT_CHECK(anv_diag_log_get(&rig.node.diag, 0, &e));
    MT_CHECK(e.code == ROVER_ERR_HARD_FAULT && e.detail == 0xBEEFu && e.severity == ROVER_SEVERITY_CRITICAL);
    MT_CHECK(anv_diag_log_get(&rig.node.diag, 1, &e) && e.code == ROVER_ERR_OVERCURRENT_A && e.detail == 39u);
    MT_CHECK(anv_diag_log_get(&rig.node.diag, 31, &e) && e.detail == 9u); /* 40 errors, oldest 9 dropped */
    for (uint16_t i = 0; i < ANV_LOG_FLASH_ENTRIES; i++) {
        MT_CHECK(anv_diag_log_get(&rig.node.diag, i, &e) && e.severity >= ROVER_SEVERITY_ERROR);
    }
}

static void test_boot_count_increments_each_boot(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    diag_rig_boot(&rig, NODE, 1000u, 1);
    MT_CHECK(rig.node.diag.boot_count == 1u);
    diag_rig_boot(&rig, NODE, 1000u, 1);
    MT_CHECK(rig.node.diag.boot_count == 2u);
    diag_rig_boot(&rig, NODE, 1000u, 1);
    MT_CHECK(rig.node.diag.boot_count == 3u);

    resp_t r[4];
    MT_CHECK(service(&rig, ANV_SERVICE_OP_INFO, 24u, r, 4, 1000u) == 1u);
    MT_CHECK(r[0].status == 0u && r[0].value == 3u);

    diag_hw_wipe(); /* a new, never-used board */
    diag_rig_boot(&rig, NODE, 1000u, 1);
    MT_CHECK(rig.node.diag.boot_count == 1u);
}

static void test_corrupt_flash_log_starts_fresh(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    anv_node_event(&rig.node, ROVER_ERR_HARD_FAULT, 1u, t);
    MT_CHECK(g_hw.log_present);

    g_hw.log[40] ^= 0x5Au; /* a flipped bit in an entry */
    diag_rig_boot(&rig, NODE, 1000u, 1);
    MT_CHECK(anv_diag_log_count(&rig.node.diag) == 0u);
    MT_CHECK(rig.node.diag.boot_count == 1u);
}

static void test_flash_is_written_rarely(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    int after_boot = g_hw.log_saves; /* the boot count */
    MT_CHECK(after_boot == 1);

    /* A burst of errors over 3 s: no flash write yet (wear), the critical one aside. */
    for (uint16_t i = 0; i < 6; i++) {
        t += 500u;
        anv_node_event(&rig.node, ROVER_ERR_OVERCURRENT_A, i, t);
        anv_node_tick(&rig.node, t);
        drain_host(&rig);
    }
    MT_CHECK(g_hw.log_saves == after_boot);

    t += 3000u; /* 6 s since the boot write */
    anv_node_tick(&rig.node, t);
    MT_CHECK(g_hw.log_saves == after_boot + 1);

    /* Nothing new to store: no more writes. */
    t += 10000u;
    anv_node_tick(&rig.node, t);
    MT_CHECK(g_hw.log_saves == after_boot + 1);
}

static void test_no_periodic_flash_write_while_armed(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    int saves = g_hw.log_saves;

    /* Keep it armed with fresh commands while an error is logged and the write interval passes. */
    anv_node_event(&rig.node, ROVER_ERR_OVERCURRENT_A, 1u, t);
    for (uint8_t i = 0; i < 80; i++) {
        t += 100u;
        acc_can_frame_t cmd = make_drive_cmd_frame(NODE, (uint8_t)(2u + i), ROVER_NODE_ORIN, 1u, 2u, 0, 0, 0);
        test_rig_host_send(&rig, &cmd);
        test_rig_pump(&rig, t);
        anv_node_tick(&rig.node, t);
        drain_host(&rig);
    }
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);
    MT_CHECK(g_hw.log_saves == saves); /* 8 s armed, nothing written */

    acc_can_frame_t disarm = make_nmt_frame(NODE, ANV_NMT_CMD_DISARM, ROVER_NODE_ORIN, 5u);
    test_rig_host_send(&rig, &disarm);
    test_rig_pump(&rig, t);
    anv_node_tick(&rig.node, t);
    MT_CHECK(g_hw.log_saves == saves + 1); /* written once it is safe to stall */

    /* A critical event is written at once even while armed. */
    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 90u, t);
    saves = g_hw.log_saves;
    anv_node_event(&rig.node, ROVER_ERR_HARD_FAULT, 1u, t + 10u);
    MT_CHECK(g_hw.log_saves == saves + 1);
}

static void test_flash_write_failure_is_reported(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    seen_event_t ev[SEEN_MAX];
    drain_host(&rig);

    g_hw.log_fail = true;
    anv_node_event(&rig.node, ROVER_ERR_HARD_FAULT, 0u, t + 10u);
    size_t n = collect_events(&rig, ev, SEEN_MAX);
    bool saw = false;
    for (size_t i = 0; i < n; i++) {
        saw = saw || ev[i].code == ROVER_ERR_FLASH_WRITE_FAIL;
    }
    MT_CHECK(saw);
}

static void test_log_read_returns_entries_newest_first(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 2);
    t += 2500u;
    anv_node_event(&rig.node, ROVER_ERR_OVERCURRENT_A, 0x1111u, t);
    t += 1000u;
    anv_node_raise(&rig.node, ROVER_ERR_DRIVER_FAULT_PIN, 0x2222u, t);
    t += 1000u;
    anv_node_lower(&rig.node, ROVER_ERR_DRIVER_FAULT_PIN, 0x3333u, t);
    drain_host(&rig);

    resp_t r[4];
    /* Entry 0 = the cleared event. Two frames: code/detail, then uptime/boot count + flags. */
    MT_CHECK(service(&rig, ANV_SERVICE_OP_LOG_READ, 0u, r, 4, t) == 2u);
    MT_CHECK(r[0].opcode == ANV_SERVICE_OP_LOG_READ && r[0].txn == 9u && r[0].status == 0u && r[0].index == 0u);
    MT_CHECK(r[0].value == (((uint32_t)ROVER_ERR_DRIVER_FAULT_PIN << 16) | 0x3333u));
    MT_CHECK(r[1].index == 0x8000u);
    MT_CHECK((r[1].status & ANV_LOG_FLAG_CLEARED) != 0u);
    MT_CHECK((r[1].value & 0xFFFFu) == 1u);              /* boot count */
    MT_CHECK((r[1].value >> 16) == (uint32_t)((t - 1000u) / 100u)); /* 0.1 s since boot */

    MT_CHECK(service(&rig, ANV_SERVICE_OP_LOG_READ, 1u, r, 4, t) == 2u);
    MT_CHECK(r[0].value == (((uint32_t)ROVER_ERR_DRIVER_FAULT_PIN << 16) | 0x2222u));
    MT_CHECK((r[1].status & ANV_LOG_FLAG_CLEARED) == 0u);

    MT_CHECK(service(&rig, ANV_SERVICE_OP_LOG_READ, 2u, r, 4, t) == 2u);
    MT_CHECK(r[0].value == (((uint32_t)ROVER_ERR_OVERCURRENT_A << 16) | 0x1111u));

    /* Past the end: one BAD_INDEX answer. */
    MT_CHECK(service(&rig, ANV_SERVICE_OP_LOG_READ, 3u, r, 4, t) == 1u);
    MT_CHECK(r[0].status == ANV_PARAM_STATUS_BAD_INDEX);
}

static void test_log_clear_wipes_ram_and_flash_but_not_while_armed(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    uint32_t t = 1000;
    diag_rig_boot(&rig, NODE, t, 1);
    anv_node_event(&rig.node, ROVER_ERR_HARD_FAULT, 1u, t);
    drain_host(&rig);
    MT_CHECK(anv_diag_log_count(&rig.node.diag) == 1u);

    test_rig_become_master_and_arm(&rig, ROVER_NODE_ORIN, 5u, 1u, t);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_ARMED);
    resp_t r[4];
    MT_CHECK(service(&rig, ANV_SERVICE_OP_LOG_CLEAR, 0u, r, 4, t) == 1u);
    MT_CHECK(r[0].status == ANV_PARAM_STATUS_NOT_ALLOWED_WHILE_ARMED);
    MT_CHECK(anv_diag_log_count(&rig.node.diag) == 1u);

    acc_can_frame_t disarm = make_nmt_frame(NODE, ANV_NMT_CMD_DISARM, ROVER_NODE_ORIN, 5u);
    test_rig_host_send(&rig, &disarm);
    test_rig_pump(&rig, t);
    MT_CHECK(service(&rig, ANV_SERVICE_OP_LOG_CLEAR, 0u, r, 4, t) == 1u);
    MT_CHECK(r[0].status == 0u);
    MT_CHECK(anv_diag_log_count(&rig.node.diag) == 0u);

    diag_rig_boot(&rig, NODE, 1000u, 1); /* and it stays cleared across a reset */
    MT_CHECK(anv_diag_log_count(&rig.node.diag) == 0u);
    MT_CHECK(rig.node.diag.boot_count == 2u); /* the boot count is kept */
}

static void test_hard_fault_record_reported_after_reset(void)
{
    diag_hw_wipe();
    g_hw.reset_cause = ANV_RESET_HARD_FAULT;
    g_hw.crash_present = true;
    g_hw.crash = (anv_crash_record_t){.pc = 0x08001234u, .lr = 0x08005678u, .xpsr = 0x61000000u,
                                      .cfsr = 0x00020000u, .hfsr = 0x40000000u, .mmfar = 0x20001000u,
                                      .bfar = 0x20002000u};
    test_rig_t rig;
    diag_rig_boot(&rig, NODE, 1000u, 1);
    seen_event_t ev[SEEN_MAX];
    size_t n = collect_events(&rig, ev, SEEN_MAX);
    MT_CHECK(n == 1u);
    MT_CHECK(ev[0].code == ROVER_ERR_HARD_FAULT && ev[0].severity == ROVER_SEVERITY_CRITICAL);
    MT_CHECK(ev[0].detail == 0x1234u); /* low half of the PC; the rest is in the INFO pages */

    resp_t r[2];
    const uint32_t expect[] = {1u, 0x08001234u, 0x08005678u, 0x61000000u, 0x00020000u, 0x40000000u, 0x20001000u, 0x20002000u};
    for (uint16_t i = 0; i < 8; i++) {
        MT_CHECK(service(&rig, ANV_SERVICE_OP_INFO, (uint16_t)(16u + i), r, 2, 1000u) == 1u);
        MT_CHECK(r[0].status == 0u && r[0].value == expect[i]);
    }
    MT_CHECK(service(&rig, ANV_SERVICE_OP_INFO, 26u, r, 2, 1000u) == 1u && r[0].value == ANV_RESET_HARD_FAULT);

    /* It was in the log (flash too), and the next boot has no crash record. */
    g_hw.reset_cause = ANV_RESET_SOFTWARE;
    diag_rig_boot(&rig, NODE, 1000u, 1);
    MT_CHECK(!rig.node.crash_valid);
    anv_log_entry_t e;
    MT_CHECK(anv_diag_log_get(&rig.node.diag, 0, &e) && e.code == ROVER_ERR_HARD_FAULT && e.boot_count == 1u);
}

static void test_other_reset_causes_become_events(void)
{
    struct {
        uint8_t cause;
        uint16_t code;
    } cases[] = {
        {ANV_RESET_WATCHDOG, ROVER_ERR_WATCHDOG_RESET},
        {ANV_RESET_BROWNOUT, ROVER_ERR_BROWNOUT_RESET},
        {ANV_RESET_STACK_OVERFLOW, ROVER_ERR_STACK_OVERFLOW},
        {ANV_RESET_PANIC, ROVER_ERR_HARD_FAULT}, /* ESP32-S3 panic: same code, detail = panic PC */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        diag_hw_wipe();
        g_hw.reset_cause = cases[i].cause;
        if (cases[i].cause == ANV_RESET_PANIC) {
            g_hw.crash_present = true;
            g_hw.crash.pc = 0x420000ABu;
        }
        test_rig_t rig;
        diag_rig_boot(&rig, NODE, 1000u, 1);
        seen_event_t ev[SEEN_MAX];
        MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);
        MT_CHECK(ev[0].code == cases[i].code);
        if (cases[i].cause == ANV_RESET_PANIC) {
            MT_CHECK(ev[0].detail == 0x00ABu);
        }
    }

    /* A normal power-on or software reset is not an event. */
    diag_hw_wipe();
    g_hw.reset_cause = ANV_RESET_POWER_ON;
    test_rig_t rig;
    diag_rig_boot(&rig, NODE, 1000u, 1);
    seen_event_t ev[SEEN_MAX];
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 0u);
}

static void test_failed_self_test_reports_which_tests_and_faults_the_node(void)
{
    diag_hw_wipe();
    g_hw.self_test_mask = (1u << 1) | (1u << 4); /* tests 2 (driver fault pin) and 5 (parameter CRC) */
    test_rig_t rig;
    diag_rig_boot(&rig, NODE, 1000u, 1);
    seen_event_t ev[SEEN_MAX];
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 1u);
    MT_CHECK(ev[0].code == ROVER_ERR_SELF_TEST_FAIL && ev[0].detail == 0x12u);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_FAULT);

    resp_t r[2];
    MT_CHECK(service(&rig, ANV_SERVICE_OP_INFO, 27u, r, 2, 1000u) == 1u && r[0].value == 0x12u);

    /* A passing self-test leaves the node disarmed and quiet. */
    diag_hw_wipe();
    diag_rig_boot(&rig, NODE, 1000u, 1);
    MT_CHECK(collect_events(&rig, ev, SEEN_MAX) == 0u);
    MT_CHECK(anv_node_state(&rig.node) == ANV_STATE_DISARMED);
}

static void test_can_error_type_counts_are_in_info_pages(void)
{
    diag_hw_wipe();
    test_rig_t rig;
    diag_rig_boot(&rig, NODE, 1000u, 1);
    for (uint16_t i = 0; i < ACC_CAN_ERRTYPE_COUNT; i++) {
        rig.node_port.error_state.error_type_counts[i] = 10u + i;
    }
    anv_node_tick(&rig.node, 1010u);
    drain_host(&rig);

    resp_t r[2];
    for (uint16_t i = 0; i < ACC_CAN_ERRTYPE_COUNT; i++) {
        MT_CHECK(service(&rig, ANV_SERVICE_OP_INFO, (uint16_t)(32u + i), r, 2, 1010u) == 1u);
        MT_CHECK(r[0].status == 0u && r[0].value == 10u + i);
    }
    MT_CHECK(service(&rig, ANV_SERVICE_OP_INFO, 99u, r, 2, 1010u) == 1u);
    MT_CHECK(r[0].status == ANV_PARAM_STATUS_BAD_INDEX);
}

int main(void)
{
    MT_RUN(test_ram_ring_keeps_the_last_64);
    MT_RUN(test_flash_log_survives_reset_and_keeps_last_32_errors);
    MT_RUN(test_boot_count_increments_each_boot);
    MT_RUN(test_corrupt_flash_log_starts_fresh);
    MT_RUN(test_flash_is_written_rarely);
    MT_RUN(test_no_periodic_flash_write_while_armed);
    MT_RUN(test_flash_write_failure_is_reported);
    MT_RUN(test_log_read_returns_entries_newest_first);
    MT_RUN(test_log_clear_wipes_ram_and_flash_but_not_while_armed);
    MT_RUN(test_hard_fault_record_reported_after_reset);
    MT_RUN(test_other_reset_causes_become_events);
    MT_RUN(test_failed_self_test_reports_which_tests_and_faults_the_node);
    MT_RUN(test_can_error_type_counts_are_in_info_pages);
    return mt_summary();
}
