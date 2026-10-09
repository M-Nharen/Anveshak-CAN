/* The real bootloader (anv_bl + isotp-c + command logic, fake G431-like flash) behind a line
 * protocol on stdin/stdout, so host/tools/anv_flash's tests can flash it with the real
 * anv-flash over a python-can virtual bus (tests/test_real_bootloader.py).
 *
 *   in:  "T <ms>"                 advance the clock and poll
 *        "F <id hex> <data hex>"  a frame from the bus
 *   out: "F <id hex> <data hex>"  frames the bootloader sent
 *        "J <build id hex>"       it jumped to a valid application (the bridge plays the app)
 *        "."                      end of the reply to one input line
 *
 * Usage: bl_stdio_node <node id> <uid hex byte>. Flash starts erased; BOOT resets into a new
 * boot sequence with the same flash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acc_can_loopback.h"
#include "anv_bl.h"
#include "anv_bl_image.h"
#include "fake_flash.h"

static fake_flash_t s_ff;
static anv_bl_t s_bl;
static acc_can_loopback_bus_t s_bus;
static acc_can_loopback_port_t s_node_port;
static acc_can_loopback_port_t s_wire_port;
static acc_can_driver_t s_node_drv;
static acc_can_driver_t s_wire_drv;
static uint8_t s_uid[12];
static uint8_t s_node_id;
static uint32_t s_now;
static int s_jumped;

static void boot(void)
{
    acc_can_filter_t filters[2];
    anv_bl_can_filters(s_node_id, filters);
    (void)s_node_drv.init(s_node_drv.ctx, 500000u, filters, 2u);
    anv_bl_config_t cfg = {
        .node_id = s_node_id,
        .node_type = 1u,
        .driver = &s_node_drv,
        .flash = &s_ff.flash,
        .uid = s_uid,
        .flash_size_kb = 128u,
        .stay_flag = false,
    };
    anv_bl_init(&s_bl, &cfg, s_now);
    s_jumped = 0;
}

static void step(void)
{
    acc_can_frame_t f;
    if (s_jumped) {
        while (s_node_drv.receive(s_node_drv.ctx, &f)) {
        }
        return;
    }
    while (s_node_drv.receive(s_node_drv.ctx, &f)) {
        anv_bl_on_rx_frame(&s_bl, &f);
    }
    anv_bl_mode_t mode = anv_bl_poll(&s_bl, s_now);
    while (s_wire_drv.receive(s_wire_drv.ctx, &f)) {
        printf("F %03X ", f.id);
        for (uint8_t i = 0; i < f.dlc; i++) {
            printf("%02X", f.data[i]);
        }
        printf("\n");
    }
    if (mode == ANV_BL_MODE_JUMP) {
        anv_bl_app_header_t hdr;
        (void)anv_bl_app_check(&s_ff.flash, 1u, &hdr);
        printf("J %08X\n", (unsigned)hdr.build_id);
        s_jumped = 1;
    } else if (mode == ANV_BL_MODE_RESET) {
        boot();
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <node id> <uid byte hex>\n", argv[0]);
        return 2;
    }
    s_node_id = (uint8_t)atoi(argv[1]);
    memset(s_uid, (int)strtoul(argv[2], NULL, 16), sizeof(s_uid));
    fake_flash_init_g4(&s_ff);

    acc_can_loopback_bus_init(&s_bus);
    (void)acc_can_loopback_attach(&s_bus, &s_node_port);
    (void)acc_can_loopback_attach(&s_bus, &s_wire_port);
    s_node_drv = acc_can_loopback_make_driver(&s_node_port);
    s_wire_drv = acc_can_loopback_make_driver(&s_wire_port);
    (void)s_wire_drv.init(s_wire_drv.ctx, 500000u, NULL, 0u);
    boot();

    char line[64];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (line[0] == 'T') {
            s_now = (uint32_t)strtoul(&line[2], NULL, 10);
        } else if (line[0] == 'F') {
            char *end;
            acc_can_frame_t f;
            f.id = (uint16_t)strtoul(&line[2], &end, 16);
            const char *hex = end + 1;
            f.dlc = 0;
            while (f.dlc < 8u && hex[0] != '\0' && hex[0] != '\n' && hex[1] != '\0') {
                char byte[3] = {hex[0], hex[1], '\0'};
                f.data[f.dlc++] = (uint8_t)strtoul(byte, NULL, 16);
                hex += 2;
            }
            (void)s_wire_drv.send(s_wire_drv.ctx, &f, ACC_CAN_PRIO_SERVICE);
        }
        step();
        printf(".\n");
        fflush(stdout);
    }
    return 0;
}
