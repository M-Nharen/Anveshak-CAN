/* The bootloader end to end on Linux: boot sequence, ISO-TP on class 7 through the loopback
 * CAN driver, and the command set against a fake flash -- the same code that runs on the
 * STM32, minus the port. A simulated host (an isotp-c link on another loopback port) plays
 * anv-flash's part. Covers the four "Done when" cases: normal flash, interrupted flash (stays
 * in the bootloader), wrong node type (rejected) and CRC mismatch (rejected), plus the boot
 * window, the stay flag and rescue by UID. */
#include <stdio.h>
#include <string.h>

#include "acc_can_loopback.h"
#include "anv_bl.h"
#include "anv_bl_image.h"
#include "errors.h"
#include "fake_flash.h"
#include "ids.h"
#include "minitest.h"
#include "rover.h"
#include "test_image.h"

#define NODE_TYPE_DRIVE 1u
#define NODE_ID 4u

static const uint8_t k_uid[12] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18, 0x29, 0x3A, 0x4B, 0x5C};
static const uint8_t k_uid_other[12] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

static uint32_t s_now;
static acc_can_loopback_bus_t s_bus;
static fake_flash_t s_ff;
static fake_flash_t s_ff2;
static test_image_t s_img;

/* ---- Node side: one bootloader instance on a loopback port ---------------------------- */

typedef struct {
    acc_can_loopback_port_t port;
    acc_can_driver_t driver;
    anv_bl_t bl;
    anv_bl_mode_t mode;
    int resets;
    int jumps;
} sim_node_t;

static sim_node_t s_node;
static sim_node_t s_node2;

static void node_boot(sim_node_t *n, fake_flash_t *ff, uint8_t node_id, const uint8_t *uid, bool stay_flag)
{
    acc_can_filter_t filters[2];
    anv_bl_can_filters(node_id, filters);
    (void)n->driver.init(n->driver.ctx, 500000u, filters, 2u);
    acc_can_frame_t junk;
    while (n->driver.receive(n->driver.ctx, &junk)) {
        /* a reset loses whatever was in the controller */
    }
    anv_bl_config_t cfg = {
        .node_id = node_id,
        .node_type = NODE_TYPE_DRIVE,
        .driver = &n->driver,
        .flash = &ff->flash,
        .uid = uid,
        .flash_size_kb = 128u,
        .stay_flag = stay_flag,
    };
    anv_bl_init(&n->bl, &cfg, s_now);
    n->mode = ANV_BL_MODE_LISTEN;
}

static void node_attach(sim_node_t *n)
{
    memset(n, 0, sizeof(*n));
    (void)acc_can_loopback_attach(&s_bus, &n->port);
    n->driver = acc_can_loopback_make_driver(&n->port);
}

static void node_step(sim_node_t *n)
{
    if (n->mode == ANV_BL_MODE_JUMP || n->mode == ANV_BL_MODE_RESET) {
        return; /* the test decides what "after the jump/reset" means */
    }
    acc_can_frame_t f;
    while (n->driver.receive(n->driver.ctx, &f)) {
        anv_bl_on_rx_frame(&n->bl, &f);
    }
    n->mode = anv_bl_poll(&n->bl, s_now);
    if (n->mode == ANV_BL_MODE_JUMP) {
        n->jumps++;
    } else if (n->mode == ANV_BL_MODE_RESET) {
        n->resets++;
    }
}

/* ---- Host side: an isotp-c link per channel, plus a log of everything else it heard ---- */

typedef struct {
    anv_isotp_port_t port;
    acc_can_driver_t *driver;
    IsoTpLink link;
    uint16_t rx_id;
    uint8_t tx_buf[ANV_BL_MAX_REQUEST];
    uint8_t rx_buf[256];
} sim_host_link_t;

static acc_can_loopback_port_t s_host_port;
static acc_can_driver_t s_host_driver;
static sim_host_link_t s_host;

static int s_hb_count;
static uint8_t s_hb_last_state;
static uint16_t s_hb_last_build;
static int s_event_count;
static uint16_t s_event_last_code;
static uint8_t s_event_last_severity;

static int host_send(anv_isotp_port_t *self, uint32_t id, const uint8_t *data, uint8_t len)
{
    sim_host_link_t *h = (sim_host_link_t *)self;
    acc_can_frame_t f;
    f.id = (uint16_t)id;
    f.dlc = len;
    memcpy(f.data, data, len);
    return (h->driver->send(h->driver->ctx, &f, ACC_CAN_PRIO_SERVICE) == ACC_CAN_OK) ? ISOTP_RET_OK
                                                                                      : ISOTP_RET_NOSPACE;
}

static void host_open(uint8_t node_id)
{
    s_host.port.send = host_send;
    s_host.driver = &s_host_driver;
    s_host.rx_id = ROVER_CAN_ID(ROVER_CLASS_BOOTLOADER, node_id, 1u);
    anv_isotp_link_init(&s_host.link, &s_host.port, ROVER_CAN_ID(ROVER_CLASS_BOOTLOADER, node_id, 0u),
                        s_host.tx_buf, sizeof(s_host.tx_buf), s_host.rx_buf, sizeof(s_host.rx_buf));
}

static void host_step(void)
{
    acc_can_frame_t f;
    while (s_host_driver.receive(s_host_driver.ctx, &f)) {
        if (f.id == s_host.rx_id) {
            isotp_on_can_message(&s_host.link, f.data, f.dlc);
        } else if (ROVER_ID_CLASS(f.id) == ROVER_CLASS_HEARTBEAT) {
            struct rover_drive1_node_hb_t hb;
            if (rover_drive1_node_hb_unpack(&hb, f.data, f.dlc) == 0) {
                s_hb_count++;
                s_hb_last_state = hb.state;
                s_hb_last_build = hb.fw_build_id;
            }
        } else if (ROVER_ID_CLASS(f.id) == ROVER_CLASS_TELEMETRY && ROVER_ID_SUB(f.id) == 7u) {
            struct rover_drive1_node_event_t ev;
            if (rover_drive1_node_event_unpack(&ev, f.data, f.dlc) == 0) {
                s_event_count++;
                s_event_last_code = ev.error_code;
                s_event_last_severity = ev.severity;
            }
        }
    }
    anv_isotp_set_time_ms(s_now);
    isotp_poll(&s_host.link);
}

static void tick(void)
{
    s_now++;
    node_step(&s_node);
    node_step(&s_node2);
    host_step();
}

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        tick();
    }
}

static void clear_log(void)
{
    s_hb_count = 0;
    s_event_count = 0;
    s_event_last_code = 0u;
}

/* One request/response round trip. Returns the status byte, or 0xEE on timeout. */
static uint8_t s_resp[ANV_BL_MAX_RESPONSE];
static uint32_t s_resp_len;

static uint8_t host_request(const uint8_t *req, uint32_t len, uint32_t timeout_ms)
{
    uint8_t junk[ANV_BL_MAX_RESPONSE];
    uint32_t junk_len;
    while (isotp_receive(&s_host.link, junk, sizeof(junk), &junk_len) == ISOTP_RET_OK) {
    }
    anv_isotp_set_time_ms(s_now);
    if (isotp_send(&s_host.link, req, len) != ISOTP_RET_OK) {
        return 0xEDu;
    }
    for (uint32_t t = 0; t < timeout_ms; t++) {
        tick();
        if (isotp_receive(&s_host.link, s_resp, sizeof(s_resp), &s_resp_len) == ISOTP_RET_OK) {
            return (s_resp_len >= 2u && s_resp[0] == req[0]) ? s_resp[1] : 0xEFu;
        }
    }
    s_resp_len = 0u;
    return 0xEEu;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t resp_error(void)
{
    return (uint16_t)(s_resp[2] | (s_resp[3] << 8));
}

static uint8_t req0(uint8_t cmd)
{
    return host_request(&cmd, 1u, 200u);
}

static uint8_t req_u32(uint8_t cmd, uint32_t v)
{
    uint8_t r[5] = {cmd};
    put32(&r[1], v);
    return host_request(r, 5u, 200u);
}

static uint8_t req_erase(uint32_t off, uint32_t len)
{
    uint8_t r[9] = {ANV_BL_CMD_ERASE};
    put32(&r[1], off);
    put32(&r[5], len);
    return host_request(r, 9u, 5000u);
}

static uint8_t s_wreq[ANV_BL_MAX_REQUEST];

static uint8_t req_write(uint32_t off, const uint8_t *data, uint32_t len)
{
    s_wreq[0] = ANV_BL_CMD_WRITE;
    put32(&s_wreq[1], off);
    memcpy(&s_wreq[5], data, len);
    return host_request(s_wreq, 5u + len, 2000u);
}

/* What anv-flash does after the node is in the bootloader, stopping after `max_blocks`
 * WRITEs (to simulate an interruption). Returns the first failing status. */
static uint8_t host_flash(const test_image_t *img, uint32_t max_blocks, bool commit_and_boot)
{
    uint8_t st = req_erase(0u, img->len);
    if (st != ANV_BL_ST_OK) {
        return st;
    }
    uint32_t blocks = 0u;
    for (uint32_t off = 0; off < img->len; off += ANV_BL_MAX_WRITE) {
        if (blocks++ == max_blocks) {
            return ANV_BL_ST_OK;
        }
        uint32_t n = (img->len - off < ANV_BL_MAX_WRITE) ? img->len - off : ANV_BL_MAX_WRITE;
        st = req_write(off, &img->bytes[off], n);
        if (st != ANV_BL_ST_OK) {
            return st;
        }
    }
    st = req_u32(ANV_BL_CMD_VERIFY, img->hdr.image_crc32);
    if (st != ANV_BL_ST_OK || !commit_and_boot) {
        return st;
    }
    st = req0(ANV_BL_CMD_COMMIT);
    if (st != ANV_BL_ST_OK) {
        return st;
    }
    return req0(ANV_BL_CMD_BOOT);
}

static void setup(void)
{
    s_now = 1000u;
    acc_can_loopback_bus_init(&s_bus);
    node_attach(&s_node);
    node_attach(&s_node2);
    s_node2.mode = ANV_BL_MODE_JUMP; /* parked: not on the bus unless a test boots it */
    memset(&s_host_port, 0, sizeof(s_host_port));
    (void)acc_can_loopback_attach(&s_bus, &s_host_port);
    s_host_driver = acc_can_loopback_make_driver(&s_host_port);
    (void)s_host_driver.init(s_host_driver.ctx, 500000u, NULL, 0u);
    host_open(NODE_ID);
    fake_flash_init_g4(&s_ff);
    clear_log();
}

static void put_valid_app(fake_flash_t *ff, uint32_t seed, uint32_t build_id)
{
    test_image_build(&s_img, NODE_TYPE_DRIVE, 9000u, seed, build_id);
    memcpy(ff->mem, s_img.bytes, s_img.len);
}

/* ---- Tests ----------------------------------------------------------------------------- */

static void test_valid_app_jumps_after_window(void)
{
    setup();
    put_valid_app(&s_ff, 1u, 0x1111u);
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, false);
    run_ms(ANV_BL_LISTEN_MS - 2u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_LISTEN);
    MT_CHECK(s_hb_count == 0); /* quiet during the window */
    run_ms(5u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_JUMP);
    MT_CHECK(s_node.jumps == 1);
}

static void test_request_in_window_stays(void)
{
    setup();
    put_valid_app(&s_ff, 1u, 0x1111u);
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, false);
    run_ms(100u);
    MT_CHECK(req0(ANV_BL_CMD_INFO) == ANV_BL_ST_OK); /* the rescue path in the doc */
    run_ms(1000u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_STAY);
    MT_CHECK(s_hb_count >= 9);
    MT_CHECK(s_hb_last_state == 5u);
    MT_CHECK(s_hb_last_build == 0x1111u);
    MT_CHECK(s_event_count == 0); /* the application is fine */
}

static void test_stay_flag_then_boot(void)
{
    setup();
    put_valid_app(&s_ff, 2u, 0x2222u);
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, true);
    run_ms(500u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_STAY);
    MT_CHECK(s_hb_count >= 5);
    MT_CHECK(req0(ANV_BL_CMD_BOOT) == ANV_BL_ST_OK);
    run_ms(ANV_BL_RESET_DELAY_MS + 5u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_RESET);
}

static void test_normal_flash(void)
{
    setup();
    put_valid_app(&s_ff, 3u, 0x3333u); /* old firmware */
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, true);
    run_ms(50u);

    MT_CHECK(req0(ANV_BL_CMD_INFO) == ANV_BL_ST_OK);
    MT_CHECK(s_resp[4 + 3] == NODE_TYPE_DRIVE);

    test_image_build(&s_img, NODE_TYPE_DRIVE, 3u * ANV_BL_MAX_WRITE + 777u, 4u, 0x4444u);
    MT_CHECK(host_flash(&s_img, 1000u, true) == ANV_BL_ST_OK);
    run_ms(ANV_BL_RESET_DELAY_MS + 5u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_RESET);
    MT_CHECK(memcmp(s_ff.mem, s_img.bytes, ANV_BL_HEADER_LEN) == 0);
    MT_CHECK(memcmp(&s_ff.mem[ANV_BL_HEADER_SLOT], &s_img.bytes[ANV_BL_HEADER_SLOT], s_img.hdr.image_length) == 0);

    /* After the reset: no flag, nobody asks, the new application starts. */
    clear_log();
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, false);
    run_ms(ANV_BL_LISTEN_MS + 5u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_JUMP);
    anv_bl_app_header_t hdr;
    MT_CHECK(anv_bl_app_check(&s_ff.flash, NODE_TYPE_DRIVE, &hdr) == 0u);
    MT_CHECK(hdr.build_id == 0x4444u);
}

static void test_interrupted_flash_stays_in_bootloader(void)
{
    setup();
    put_valid_app(&s_ff, 5u, 0x5555u);
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, true);
    run_ms(50u);

    test_image_build(&s_img, NODE_TYPE_DRIVE, 6u * ANV_BL_MAX_WRITE, 6u, 0x6666u);
    MT_CHECK(host_flash(&s_img, 3u, false) == ANV_BL_ST_OK); /* cable pulled after 3 blocks */

    /* Power cycle: no flag, nobody listening. The half-written image has no header. */
    clear_log();
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, false);
    run_ms(ANV_BL_LISTEN_MS + 500u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_STAY);
    MT_CHECK(s_node.jumps == 0);
    MT_CHECK(s_hb_count >= 5 && s_hb_last_state == 5u && s_hb_last_build == 0u);
    MT_CHECK(s_event_count == 1);
    MT_CHECK(s_event_last_code == ROVER_ERR_APP_CRC_FAIL);
    MT_CHECK(s_event_last_severity == ROVER_SEVERITY_CRITICAL);

    /* And it can still be flashed from there. */
    MT_CHECK(host_flash(&s_img, 1000u, true) == ANV_BL_ST_OK);
    run_ms(ANV_BL_RESET_DELAY_MS + 5u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_RESET);
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, false);
    run_ms(ANV_BL_LISTEN_MS + 5u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_JUMP);
}

static void test_wrong_node_type_rejected(void)
{
    setup();
    put_valid_app(&s_ff, 7u, 0x7777u);
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, true);
    run_ms(50u);

    test_image_build(&s_img, 2u /* arm firmware */, 2u * ANV_BL_MAX_WRITE, 8u, 0x8888u);
    MT_CHECK(host_flash(&s_img, 1000u, true) == ANV_BL_ST_WRONG_NODE_TYPE);
    MT_CHECK(resp_error() == ROVER_ERR_WRONG_NODE_TYPE);
    MT_CHECK(req0(ANV_BL_CMD_COMMIT) == ANV_BL_ST_BAD_SEQUENCE);
    /* The ERASE before it already removed the old application, so the node stays put. */
    MT_CHECK(req0(ANV_BL_CMD_BOOT) == ANV_BL_ST_BAD_IMAGE);
    run_ms(100u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_STAY);
    MT_CHECK(anv_bl_flash_is_erased(&s_ff.flash, ANV_BL_HEADER_SLOT, s_img.hdr.image_length));
}

static void test_crc_mismatch_rejected(void)
{
    setup();
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, true);
    run_ms(50u);

    test_image_build(&s_img, NODE_TYPE_DRIVE, 2u * ANV_BL_MAX_WRITE + 100u, 9u, 0x9999u);
    s_img.bytes[ANV_BL_HEADER_SLOT + ANV_BL_MAX_WRITE + 3u] ^= 0x80u;
    MT_CHECK(host_flash(&s_img, 1000u, false) == ANV_BL_ST_MISMATCH);
    MT_CHECK(resp_error() == ROVER_ERR_APP_CRC_FAIL);
    MT_CHECK(req0(ANV_BL_CMD_COMMIT) == ANV_BL_ST_BAD_SEQUENCE);
    MT_CHECK(req0(ANV_BL_CMD_BOOT) == ANV_BL_ST_BAD_IMAGE);

    /* Even after a reset it never jumps to the corrupt image. */
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, false);
    run_ms(ANV_BL_LISTEN_MS + 200u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_STAY);
}

static void test_rescue_by_uid(void)
{
    setup();
    /* Two boards with no valid firmware: one on its own ID, one unconfigured (node 31). */
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, false);
    fake_flash_init_g4(&s_ff2);
    node_boot(&s_node2, &s_ff2, ROVER_NODE_UNCONFIGURED, k_uid_other, false);
    run_ms(ANV_BL_LISTEN_MS + 50u);
    MT_CHECK(s_node.mode == ANV_BL_MODE_STAY && s_node2.mode == ANV_BL_MODE_STAY);

    host_open(ROVER_NODE_UNCONFIGURED);
    /* Node 31 answers nothing until selected. */
    MT_CHECK(req0(ANV_BL_CMD_INFO) == 0xEEu);
    /* Unknown UID: everyone stays silent. */
    MT_CHECK(req_u32(ANV_BL_CMD_SELECT, 0x12345678u) == 0xEEu);

    /* Select the node-4 board by UID over the rescue channel and flash it there. */
    MT_CHECK(req_u32(ANV_BL_CMD_SELECT, anv_bl_uid32(k_uid)) == ANV_BL_ST_OK);
    MT_CHECK(s_resp[4] == NODE_ID);
    MT_CHECK(req0(ANV_BL_CMD_INFO) == ANV_BL_ST_OK);
    MT_CHECK(memcmp(&s_resp[4 + 4], k_uid, 12u) == 0);
    test_image_build(&s_img, NODE_TYPE_DRIVE, ANV_BL_MAX_WRITE + 10u, 10u, 0xAAAAu);
    MT_CHECK(host_flash(&s_img, 1000u, true) == ANV_BL_ST_OK);
    MT_CHECK(anv_bl_app_check(&s_ff.flash, NODE_TYPE_DRIVE, NULL) == 0u);
    MT_CHECK(anv_bl_flash_is_erased(&s_ff2.flash, 0u, s_img.len)); /* the other board untouched */

    /* The unconfigured board, selected by its own UID. */
    MT_CHECK(req_u32(ANV_BL_CMD_SELECT, anv_bl_uid32(k_uid_other)) == ANV_BL_ST_OK);
    MT_CHECK(s_resp[4] == ROVER_NODE_UNCONFIGURED);
    MT_CHECK(req0(ANV_BL_CMD_INFO) == ANV_BL_ST_OK);
    MT_CHECK(memcmp(&s_resp[4 + 4], k_uid_other, 12u) == 0);
}

static void test_select_for_other_board_does_not_hold_it(void)
{
    setup();
    put_valid_app(&s_ff, 11u, 0xBBBBu);
    node_boot(&s_node, &s_ff, NODE_ID, k_uid, false);
    host_open(ROVER_NODE_UNCONFIGURED);
    run_ms(50u);
    /* A rescue sweep looking for some other board must not keep a healthy node from booting. */
    MT_CHECK(req_u32(ANV_BL_CMD_SELECT, anv_bl_uid32(k_uid_other)) == 0xEEu);
    run_ms(ANV_BL_LISTEN_MS);
    MT_CHECK(s_node.mode == ANV_BL_MODE_JUMP);
}

int main(void)
{
    MT_RUN(test_valid_app_jumps_after_window);
    MT_RUN(test_request_in_window_stays);
    MT_RUN(test_stay_flag_then_boot);
    MT_RUN(test_normal_flash);
    MT_RUN(test_interrupted_flash_stays_in_bootloader);
    MT_RUN(test_wrong_node_type_rejected);
    MT_RUN(test_crc_mismatch_rejected);
    MT_RUN(test_rescue_by_uid);
    MT_RUN(test_select_for_other_board_does_not_hold_it);
    return mt_summary();
}
