/* The bootloader proper: boot sequence, ISO-TP transport on class 7 and heartbeats, on top of
 * the command logic (anv_bl_cmd.h). Hardware-free like the core library: a port supplies
 * the CAN driver, the flash backend, the stay flag, the UID and the clock, then calls
 * anv_bl_poll() in a loop and acts on the mode it returns (jump or reset).
 *
 * Boot sequence (docs/architecture.md, "STM32 boot sequence"):
 *   1. Outputs are never touched (hardware pull-downs keep motor enables low).
 *   2. The port reads the node ID switch and starts CAN at 500 kbps.
 *   3. Stay flag set (application received ENTER_BOOTLOADER) -> stay and wait for the host.
 *   4. Otherwise listen ANV_BL_LISTEN_MS for a request to this node (or a matching SELECT on
 *      the rescue channel). Any request the bootloader answers keeps it in the bootloader.
 *   5. Check the application header: magic, node type, length, CRC32 of the whole image.
 *   6. Valid -> jump. Invalid -> stay, report the error once as a NODE_EVENT, and send
 *      NODE_HB with state BOOTLOADER so the dashboard shows the node needs flashing.
 *
 * Channels: this node's class-7 pair (sub 0 in, sub 1 out) and, for boards whose node ID is
 * unknown or broken, the node-31 "rescue" pair, where nothing but SELECT is answered until a
 * SELECT names this board's UID. The bootloader's two hardware filters cover exactly these
 * two channels; it acts on no class-0 frame, so it does not need the application's class-0
 * filter. */
#ifndef ANV_BL_H
#define ANV_BL_H

#include <stdbool.h>
#include <stdint.h>

#include "acc_can.h"
#include "anv_bl_cmd.h"
#include "anv_heartbeat.h"
#include "anv_isotp_port.h"
#include "anv_txqueue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_BL_LISTEN_MS 300u
#define ANV_BL_HB_RATE_HZ 10u /* NODE_HB cycle time in defs/rover.dbc */
#define ANV_BL_PROTOCOL_VERSION 1u
#define ANV_BL_RESET_DELAY_MS 20u /* lets the BOOT response leave the CAN controller */
#define ANV_BL_TXQ_PER_PRIO 16u

typedef enum {
    ANV_BL_MODE_LISTEN = 0, /* waiting out the rescue window */
    ANV_BL_MODE_STAY,       /* serving the host */
    ANV_BL_MODE_JUMP,       /* port: start the application now */
    ANV_BL_MODE_RESET,      /* port: reset the MCU now (after BOOT) */
} anv_bl_mode_t;

typedef struct {
    uint8_t node_id;
    uint8_t node_type;
    const acc_can_driver_t *driver; /* already initialised with anv_bl_can_filters() */
    const anv_bl_flash_t *flash;
    const uint8_t *uid; /* 12 bytes */
    uint16_t flash_size_kb;
    bool stay_flag; /* read and cleared by the port */
    void (*feed_watchdog)(void *ctx);
    void *feed_ctx;
} anv_bl_config_t;

typedef struct {
    anv_isotp_port_t port; /* first member: isotp-c hands this pointer back to us */
    struct anv_bl_s *bl;
    IsoTpLink link;
    uint16_t rx_id;
    bool needs_select;
    uint8_t rx_buf[ANV_BL_MAX_REQUEST];
    uint8_t tx_buf[ANV_BL_MAX_RESPONSE];
} anv_bl_channel_t;

typedef struct anv_bl_s {
    anv_bl_cmd_t cmd;
    const acc_can_driver_t *driver;
    anv_txqueue_t txq;
    acc_can_frame_t txq_storage[ACC_CAN_PRIO_COUNT * ANV_BL_TXQ_PER_PRIO];

    anv_bl_channel_t own;
    anv_bl_channel_t rescue;
    bool has_rescue; /* false when this node *is* node 31: one channel serves both */

    anv_bl_mode_t mode;
    uint32_t start_ms;
    uint32_t last_ms;
    uint16_t boot_status; /* errors.yaml code from the last application check, 0 = valid */
    uint16_t app_build_id;
    anv_rate_sched_t hb_sched;
    uint8_t event_seq;
    bool reset_pending;
    uint32_t reset_at_ms;

    uint8_t req[ANV_BL_MAX_REQUEST];
    uint8_t resp[ANV_BL_MAX_RESPONSE];
} anv_bl_t;

/* The two hardware filters the port must program before anv_bl_init(). */
void anv_bl_can_filters(uint8_t node_id, acc_can_filter_t filters[2]);

void anv_bl_init(anv_bl_t *bl, const anv_bl_config_t *cfg, uint32_t now_ms);

/* Feed one received frame (frames for other IDs are ignored). */
void anv_bl_on_rx_frame(anv_bl_t *bl, const acc_can_frame_t *frame);

/* Run timers, ISO-TP, commands and heartbeats; drains the TX queue. Returns what the port
 * must do next. */
anv_bl_mode_t anv_bl_poll(anv_bl_t *bl, uint32_t now_ms);

/* The port found the application unfit to start after all (its vector table points outside
 * RAM or the image): stay in the bootloader and report `error_code` as for a bad header. */
void anv_bl_refuse_jump(anv_bl_t *bl, uint16_t error_code, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* ANV_BL_H */
