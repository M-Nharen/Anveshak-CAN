/* anv_node OTA: image transfer, a corrupted image being rejected, and the rollback that follows
 * an update that never confirms itself. Drives the wire protocol directly (anv_node_ota.h), the
 * way anv-flash would, through the fake host's ISO-TP channel (node_harness.h).
 */
#include <string.h>

#include "anv_motor_controller.h"
#include "minitest.h"
#include "node_harness.h"

#define NODE 10
#define TERM 4
#define TYPE_ID ANV_MOTOR_CONTROLLER_TYPE_ID
#define CHIP 1

static anv_node_t *n;
static h_ota_t *ota;

static void boot(void)
{
    h_reset();
    anv_node_config_t *c = h_config(0, &anv_motor_controller_type);
    c->fixed_id = NODE;
    c->info.chip = CHIP;
    MT_CHECK(h_start(0));
    n = &h_nodes[0].node;
    ota = &h_nodes[0].ota;
    h_transfer_bind(NODE);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t crc32_of(const uint8_t *p, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (unsigned b = 0; b < 8u; b++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static bool send_and_ack(const uint8_t *body, uint16_t len, uint8_t *status, uint32_t *next_offset)
{
    h_transfer_last_rx.got = false;
    h_transfer_send(body, len);
    for (int t = 0; t < 200 && !h_transfer_last_rx.got; t++) {
        h_step(1);
    }
    if (!h_transfer_last_rx.got || h_transfer_last_rx.len < 7u) {
        return false;
    }
    const uint8_t *r = h_transfer_last_rx.data;
    if (r[0] != 1u /* ANV_TRANSFER_SERVICE_OTA */ || r[1] != 4u /* ACK */) {
        return false;
    }
    *status = r[2];
    *next_offset = (uint32_t)r[3] | ((uint32_t)r[4] << 8) | ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 24);
    return true;
}

static bool begin(uint16_t type_id, uint8_t chip, uint32_t image_len, uint32_t crc, uint8_t *status)
{
    uint8_t body[13];
    body[0] = 1; /* OTA */
    body[1] = 0; /* BEGIN */
    body[2] = (uint8_t)type_id;
    body[3] = (uint8_t)(type_id >> 8);
    body[4] = chip;
    put_u32(body + 5, image_len);
    put_u32(body + 9, crc);
    uint32_t next_offset;
    return send_and_ack(body, sizeof body, status, &next_offset);
}

static bool data(uint32_t offset, const uint8_t *chunk, uint16_t len, uint8_t *status, uint32_t *next_offset)
{
    uint8_t body[6 + 64];
    body[0] = 1;
    body[1] = 1; /* DATA */
    put_u32(body + 2, offset);
    memcpy(body + 6, chunk, len);
    return send_and_ack(body, (uint16_t)(6u + len), status, next_offset);
}

static bool end(uint8_t *status, uint32_t *next_offset)
{
    const uint8_t body[2] = {1, 2}; /* OTA, END */
    return send_and_ack(body, sizeof body, status, next_offset);
}

static void abort_session(void)
{
    const uint8_t body[2] = {1, 3}; /* OTA, ABORT */
    h_transfer_send(body, sizeof body);
    h_step(2);
}

static bool flash(const uint8_t *image, uint16_t len, uint8_t *end_status)
{
    uint8_t status;
    if (!begin(TYPE_ID, CHIP, len, crc32_of(image, len), &status) || status != 0u) {
        return false;
    }
    uint32_t next = 0;
    if (!data(0, image, len, &status, &next) || status != 0u) {
        return false;
    }
    uint32_t end_next;
    return end(end_status, &end_next);
}

/* The full real sequence: flash, the (real) reboot that follows a commit, then enough uptime
 * on the new image for anv_node to confirm it on its own. */
static void test_full_transfer_and_confirm(void)
{
    boot();
    const int resets_before = h_nodes[0].app.resets;
    const uint8_t image[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t end_status = 0xFF;
    MT_CHECK(flash(image, sizeof image, &end_status));
    MT_CHECK(end_status == 0u);
    MT_CHECK(ota->commits == 1);
    MT_CHECK(ota->active_version == 1u);
    MT_CHECK(h_nodes[0].app.resets == resets_before + 1); /* on_reset: real hardware reboots here */

    MT_CHECK(h_start(0)); /* the reboot into the new image */
    n = &h_nodes[0].node;
    h_step(1);
    MT_CHECK(ota->confirms == 0); /* not yet: it hasn't run long enough */

    h_step(ANV_NODE_OTA_CONFIRM_MS - 10u);
    h_clear_log(); /* streams keep going; h_log is small and only the confirm matters now */
    h_step(110u);
    MT_CHECK(ota->confirms == 1);
    MT_CHECK(ota->rollbacks == 0);
    anv_event_t ev;
    const anv_link_frame_t *f = h_last(ANV_EVENT_PORT, NODE);
    MT_CHECK(f != NULL && anv_event_unpack(&ev, f->data, f->len) && ev.code == ANV_EVENT_UPDATE_DONE);
}

static void test_wrong_type_refused(void)
{
    boot();
    uint8_t status;
    MT_CHECK(begin(TYPE_ID + 1, CHIP, 10, 0, &status));
    MT_CHECK(status == 1u /* BAD_TYPE */);
    anv_event_t ev;
    const anv_link_frame_t *f = h_last(ANV_EVENT_PORT, NODE);
    MT_CHECK(f != NULL && anv_event_unpack(&ev, f->data, f->len) && ev.code == ANV_EVENT_UPDATE_WRONG_TYPE);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE); /* refused before it ever started */
}

static void test_refused_while_armed(void)
{
    boot();
    h_step_with_leader(60, TERM);
    h_nmt(NODE, ANV_NMT_COMMAND_ARM, TERM);
    h_step(1);
    MT_CHECK(anv_node_armed(n));
    uint8_t status;
    MT_CHECK(begin(TYPE_ID, CHIP, 10, 0, &status));
    MT_CHECK(status == 3u /* NOT_DISARMED */);
}

static void test_corrupted_image_rejected(void)
{
    boot();
    const uint8_t image[] = {9, 9, 9, 9};
    uint8_t status;
    MT_CHECK(begin(TYPE_ID, CHIP, sizeof image, 0xDEADBEEFu /* wrong on purpose */, &status));
    MT_CHECK(status == 0u);
    uint32_t next;
    MT_CHECK(data(0, image, sizeof image, &status, &next));
    MT_CHECK(status == 0u && next == sizeof image);
    MT_CHECK(end(&status, &next));
    MT_CHECK(status == 6u /* CRC_MISMATCH */);
    MT_CHECK(ota->commits == 0 && ota->aborts == 1);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE); /* back to normal, still the old image */

    /* The session is over: a second END with no new BEGIN is refused. */
    MT_CHECK(end(&status, &next));
    MT_CHECK(status == 7u /* NO_SESSION */);
}

static void test_bad_offset_and_too_big(void)
{
    boot();
    const uint8_t chunk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t status;
    MT_CHECK(begin(TYPE_ID, CHIP, 10, 0, &status) && status == 0u);
    uint32_t next;
    MT_CHECK(data(0, chunk, 8, &status, &next) && status == 0u && next == 8u);
    MT_CHECK(data(5, chunk, 2, &status, &next));    /* not where it should resume */
    MT_CHECK(status == 4u /* BAD_OFFSET */ && next == 8u);

    boot();
    MT_CHECK(begin(TYPE_ID, CHIP, 4, 0, &status) && status == 0u); /* image_len = 4 */
    MT_CHECK(data(0, chunk, 8, &status, &next));                    /* chunk is bigger than that */
    MT_CHECK(status == 2u /* TOO_BIG */);
    MT_CHECK(ota->aborts == 1);
}

static void test_write_failure_aborts(void)
{
    boot();
    h_nodes[0].ota.fail_writes = true;
    uint8_t status;
    MT_CHECK(begin(TYPE_ID, CHIP, 8, 0, &status) && status == 0u);
    const uint8_t chunk[8] = {0};
    uint32_t next;
    MT_CHECK(data(0, chunk, 8, &status, &next));
    MT_CHECK(status == 5u /* WRITE_FAILED */);
    MT_CHECK(ota->aborts == 1);
}

static void test_abort_message(void)
{
    boot();
    uint8_t status;
    MT_CHECK(begin(TYPE_ID, CHIP, 8, 0, &status) && status == 0u);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_UPDATING);
    abort_session();
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE);
    MT_CHECK(ota->aborts == 1);
}

static void test_session_timeout(void)
{
    boot();
    uint8_t status;
    MT_CHECK(begin(TYPE_ID, CHIP, 8, 0, &status) && status == 0u);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_UPDATING);
    h_step(ANV_NODE_OTA_SESSION_TIMEOUT_MS + 100u);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_IDLE);
    MT_CHECK(ota->aborts == 1);
}

static void test_estop_cancels_update(void)
{
    boot();
    uint8_t status;
    MT_CHECK(begin(TYPE_ID, CHIP, 8, 0, &status) && status == 0u);
    h_estop(5);
    h_step(1);
    MT_CHECK(anv_node_state(n) == ANV_NODE_STATE_ESTOP);
    MT_CHECK(ota->aborts == 1);
}

/* The heart of the safety story: a committed image that never calls confirm() (it keeps
 * crashing before anv_node's own confirm timer fires) is rolled back on the second boot. */
static void test_rollback_after_unconfirmed_reboot(void)
{
    boot();
    const uint8_t image[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t end_status;
    MT_CHECK(flash(image, sizeof image, &end_status) && end_status == 0u);
    MT_CHECK(ota->commits == 1 && ota->active_version == 1u);

    /* First reboot since the update: give it a chance (no rollback yet). */
    h_clear_log();
    MT_CHECK(h_start(0));
    h_step(1);
    n = &h_nodes[0].node;
    MT_CHECK(ota->rollbacks == 0);
    MT_CHECK(ota->active_version == 1u);

    /* It crashes again before anv_node's confirm timer ever fires: still unconfirmed on this
     * second boot means it didn't survive. */
    MT_CHECK(h_start(0));
    h_step(1);
    n = &h_nodes[0].node;
    MT_CHECK(ota->rollbacks == 1);
    MT_CHECK(ota->active_version == 0u); /* back to the old image */
    anv_event_t ev;
    const anv_link_frame_t *f = h_last(ANV_EVENT_PORT, NODE);
    MT_CHECK(f != NULL && anv_event_unpack(&ev, f->data, f->len) && ev.code == ANV_EVENT_UPDATE_ROLLBACK);

    /* A third boot has nothing left to roll back (the marker was cleared): no-op. */
    MT_CHECK(h_start(0));
    h_step(1);
    MT_CHECK(ota->rollbacks == 1);
}

static void test_confirm_before_reboot_keeps_the_image(void)
{
    boot();
    const uint8_t image[] = {1, 2, 3, 4};
    uint8_t end_status;
    MT_CHECK(flash(image, sizeof image, &end_status) && end_status == 0u);

    MT_CHECK(h_start(0)); /* the reboot into the new image */
    n = &h_nodes[0].node;
    h_step(ANV_NODE_OTA_CONFIRM_MS + 100u); /* it runs long enough to confirm itself */
    MT_CHECK(ota->confirms == 1);

    MT_CHECK(h_start(0)); /* reboot again, same (now confirmed) image: nothing to roll back to */
    MT_CHECK(ota->rollbacks == 0);
    MT_CHECK(ota->active_version == 1u);
}

int main(void)
{
    MT_RUN(test_full_transfer_and_confirm);
    MT_RUN(test_wrong_type_refused);
    MT_RUN(test_refused_while_armed);
    MT_RUN(test_corrupted_image_rejected);
    MT_RUN(test_bad_offset_and_too_big);
    MT_RUN(test_write_failure_aborts);
    MT_RUN(test_abort_message);
    MT_RUN(test_session_timeout);
    MT_RUN(test_estop_cancels_update);
    MT_RUN(test_rollback_after_unconfirmed_reboot);
    MT_RUN(test_confirm_before_reboot_keeps_the_image);
    return mt_summary();
}
