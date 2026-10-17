/* ISO-TP (ISO 15765-2) over the v2 identifier, for transfers longer than one frame.
 *
 * One anv_isotp_t is one channel between this node (`self`) and one peer on one port (normally
 * protocol.yaml's `transfer` port). Data frames go to the peer; flow control comes back on the
 * same port the other way. Both directions can run at once. Classic CAN: up to 4095 bytes,
 * frames always padded to 8 bytes.
 *
 * No allocation and no clock of its own: the caller passes the time in microseconds, feeds
 * received frames to anv_isotp_on_frame() and calls anv_isotp_poll() every loop. The data
 * passed to anv_isotp_send() must stay valid until on_tx_done runs.
 *
 * ISO-TP has no end-to-end acknowledgement: with block_size 0 a sender can finish while the
 * receiver dropped the transfer. Protocols on top (OTA, logs) confirm with their own replies.
 */
#ifndef ANV_ISOTP_H
#define ANV_ISOTP_H

#include "anv_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_ISOTP_MAX_LEN 4095u
#define ANV_ISOTP_SF_MAX 7u
#define ANV_ISOTP_MAX_BURST 8u /* consecutive frames sent per poll when the peer allows STmin 0 */

/* Defaults; all of them are settings. */
#define ANV_ISOTP_DEFAULT_TIMEOUT_MS 1000u /* N_Bs and N_Cr */
#define ANV_ISOTP_DEFAULT_BLOCK_SIZE 0u    /* 0 = all consecutive frames after one flow control */
#define ANV_ISOTP_DEFAULT_ST_MIN 0xF5u     /* ask for >= 500 us between frames */
#define ANV_ISOTP_DEFAULT_MAX_WAIT 10u     /* flow-control WAITs accepted in a row */
#define ANV_ISOTP_DEFAULT_PADDING 0xAAu

typedef enum {
    ANV_ISOTP_OK = 0,
    ANV_ISOTP_ERR_ARG,          /* bad length or configuration */
    ANV_ISOTP_ERR_BUSY,         /* a transfer in this direction is already running */
    ANV_ISOTP_ERR_TIMEOUT_FC,   /* N_Bs: no flow control from the receiver */
    ANV_ISOTP_ERR_TIMEOUT_CF,   /* N_Cr: the next consecutive frame never came */
    ANV_ISOTP_ERR_TX_STALLED,   /* the link refused our frames for a whole timeout */
    ANV_ISOTP_ERR_WRONG_SN,     /* a consecutive frame was lost, repeated or out of order */
    ANV_ISOTP_ERR_OVERFLOW,     /* the transfer doesn't fit the receiver's buffer */
    ANV_ISOTP_ERR_WAIT_LIMIT,   /* too many flow-control WAITs */
    ANV_ISOTP_ERR_INTERRUPTED,  /* a new transfer started before this one ended */
    ANV_ISOTP_ERR_BAD_FRAME,    /* malformed frame (length, PCI, flow status) */
    ANV_ISOTP_ERR_ABORTED,      /* anv_isotp_abort() */
} anv_isotp_result_t;

typedef struct {
    anv_node_id_t self;
    anv_node_id_t peer;
    uint16_t port;
    uint8_t priority;

    uint8_t block_size;    /* asked of the peer when receiving */
    uint8_t st_min;        /* asked of the peer when receiving, raw ISO-TP byte */
    uint16_t timeout_ms;
    uint8_t max_wait;
    uint8_t padding;

    uint8_t *rx_buf;       /* where received transfers are assembled */
    uint16_t rx_buf_size;

    /* Queue one frame; false if the link can't take it now (retried on the next poll). */
    bool (*send)(void *user, const anv_link_frame_t *frame);
    /* A transfer arrived. `data` is rx_buf, valid until the next frame is fed in. */
    void (*on_rx)(void *user, const uint8_t *data, uint16_t len);
    /* Optional: our transfer finished (ANV_ISOTP_OK) or failed. */
    void (*on_tx_done)(void *user, anv_isotp_result_t result);
    /* Optional: an incoming transfer was dropped. */
    void (*on_rx_error)(void *user, anv_isotp_result_t result);
    void *user;
} anv_isotp_config_t;

typedef struct {
    uint32_t tx_done;
    uint32_t tx_failed;
    uint32_t rx_done;
    uint32_t rx_failed;
    uint32_t rx_wrong_sn;
    uint32_t rx_stray;  /* consecutive or flow-control frames nobody was waiting for */
} anv_isotp_stats_t;

typedef enum { ANV_ISOTP_TX_IDLE, ANV_ISOTP_TX_SEND_FIRST, ANV_ISOTP_TX_WAIT_FC, ANV_ISOTP_TX_SEND_CF } anv_isotp_tx_state_t;

typedef struct {
    anv_isotp_config_t cfg;
    anv_isotp_stats_t stats;

    /* sending */
    anv_isotp_tx_state_t tx_state;
    const uint8_t *tx_data;
    uint16_t tx_len;
    uint16_t tx_off;
    uint8_t tx_sn;
    uint8_t tx_bs;        /* peer's block size, 0 = unlimited */
    uint8_t tx_bs_left;
    uint8_t tx_waits;
    uint32_t tx_st_us;    /* peer's STmin */
    uint32_t tx_next_us;  /* earliest time for the next consecutive frame */
    uint32_t tx_deadline_us;

    /* receiving */
    bool rx_active;
    uint16_t rx_len;
    uint16_t rx_got;
    uint8_t rx_sn;
    uint8_t rx_bs_left;
    uint32_t rx_deadline_us;
    bool rx_fc_pending;   /* a flow-control frame the link couldn't take yet */
    uint8_t rx_fc_status;
} anv_isotp_t;

/* Defaults include port and priority of protocol.yaml's `transfer` message; set self, peer,
 * rx_buf and the callbacks. */
void anv_isotp_config_default(anv_isotp_config_t *cfg);
/* ANV_ISOTP_ERR_ARG if send, on_rx or rx_buf is missing, or an address doesn't fit. */
anv_isotp_result_t anv_isotp_init(anv_isotp_t *t, const anv_isotp_config_t *cfg);

/* Start sending `len` (1..4095) bytes. The result comes later through on_tx_done. */
anv_isotp_result_t anv_isotp_send(anv_isotp_t *t, const uint8_t *data, uint16_t len, uint32_t now_us);

/* Feed every received frame. True if it belonged to this channel (port, peer -> self). */
bool anv_isotp_on_frame(anv_isotp_t *t, const anv_link_frame_t *frame, uint32_t now_us);

/* Call every loop: sends pending frames and handles timeouts. */
void anv_isotp_poll(anv_isotp_t *t, uint32_t now_us);

void anv_isotp_abort(anv_isotp_t *t);
bool anv_isotp_tx_busy(const anv_isotp_t *t);
bool anv_isotp_rx_busy(const anv_isotp_t *t);

/* STmin byte -> microseconds (reserved values mean the longest, 127 ms, as ISO 15765-2 says). */
uint32_t anv_isotp_st_min_us(uint8_t st_min);

const char *anv_isotp_result_name(anv_isotp_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* ANV_ISOTP_H */
