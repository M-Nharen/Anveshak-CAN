/* anv_link: a small CAN driver interface for the v2 libraries.
 *
 * Classic CAN, 29-bit extended identifiers only (schema/protocol.yaml). One backend vtable
 * (ESP32 TWAI, or the in-process loopback bus for Linux tests) under one front end that keeps
 * the parts every backend would otherwise repeat: exact acceptance filtering, counters, and
 * bus-off tracking with automatic recovery. No dynamic allocation, no RTOS dependency.
 */
#ifndef ANV_LINK_H
#define ANV_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_LINK_MAX_DLC 8u
#define ANV_LINK_MAX_FILTERS 4u
#define ANV_LINK_EXT_ID_MASK 0x1FFFFFFFu

/* Settings, not constants: these are only the defaults (CLAUDE.md). */
#define ANV_LINK_DEFAULT_BITRATE 500000u
#define ANV_LINK_DEFAULT_RECOVER_RETRY_MS 1000u

typedef struct {
    uint32_t id; /* 29-bit extended identifier */
    uint8_t len; /* 0..8 */
    uint8_t data[ANV_LINK_MAX_DLC];
} anv_link_frame_t;

/* A frame is accepted if (id & mask) == (filter.id & mask) for at least one filter.
 * No filters means accept everything. */
typedef struct {
    uint32_t id;
    uint32_t mask;
} anv_link_filter_t;

typedef enum {
    ANV_LINK_STATE_ACTIVE = 0,
    ANV_LINK_STATE_PASSIVE = 1,
    ANV_LINK_STATE_BUS_OFF = 2,
} anv_link_state_t;

typedef enum {
    ANV_LINK_OK = 0,
    ANV_LINK_ERR_ARG = -1,
    ANV_LINK_ERR_TX_FULL = -2,   /* try again later */
    ANV_LINK_ERR_NOT_READY = -3, /* not started, or bus-off */
    ANV_LINK_ERR_BACKEND = -4,
} anv_link_status_t;

typedef struct {
    uint32_t bitrate;
    anv_link_filter_t filters[ANV_LINK_MAX_FILTERS];
    uint8_t n_filters;
    bool auto_recover;          /* start bus-off recovery by itself in anv_link_poll() */
    uint32_t recover_retry_ms;  /* retry recovery this often while still bus-off */
} anv_link_config_t;

/* What a backend reports; counters are cumulative since start. */
typedef struct {
    anv_link_state_t state;
    uint8_t tec;
    uint8_t rec;
    uint32_t rx_overflow; /* frames lost because the backend's receive queue was full */
    uint32_t tx_failed;   /* frames the controller accepted but could not send */
} anv_link_hw_status_t;

typedef struct {
    anv_link_state_t state;
    uint8_t tec;
    uint8_t rec;
    uint32_t tx_frames;    /* handed to the backend */
    uint32_t tx_rejected;  /* anv_link_send() refused (queue full, bus-off, ...) */
    uint32_t tx_failed;
    uint32_t rx_frames;    /* passed the filters and returned to the caller */
    uint32_t rx_filtered;  /* dropped by the software filters */
    uint32_t rx_overflow;
    uint32_t bus_off_count;
    uint32_t recoveries;   /* times the link came back from bus-off */
} anv_link_stats_t;

/* Backend vtable. `ctx` is passed back to every call. Backends may filter in hardware
 * (a superset is fine: the front end applies the exact filters again). */
typedef struct {
    int (*start)(void *ctx, const anv_link_config_t *cfg);
    int (*stop)(void *ctx);
    int (*set_filters)(void *ctx, const anv_link_filter_t *filters, uint8_t n);
    int (*send)(void *ctx, const anv_link_frame_t *frame);
    bool (*receive)(void *ctx, anv_link_frame_t *frame);
    int (*get_status)(void *ctx, anv_link_hw_status_t *status);
    int (*recover)(void *ctx);
} anv_link_ops_t;

/* Events anv_link_poll() reports, so the caller can raise the protocol's can_* events. */
#define ANV_LINK_EV_PASSIVE    0x01u /* entered error-passive */
#define ANV_LINK_EV_BUS_OFF    0x02u /* entered bus-off */
#define ANV_LINK_EV_RECOVERED  0x04u /* back from bus-off */
#define ANV_LINK_EV_RX_OVERFLOW 0x08u /* frames were lost since the last poll */
#define ANV_LINK_EV_TX_FAILED  0x10u /* frames failed since the last poll */

typedef struct {
    const anv_link_ops_t *ops;
    void *ctx;
    anv_link_config_t cfg;
    anv_link_stats_t stats;
    bool started;
    bool recovering;
    uint32_t recover_at_ms;
} anv_link_t;

void anv_link_config_default(anv_link_config_t *cfg);

void anv_link_init(anv_link_t *link, const anv_link_ops_t *ops, void *ctx);
int anv_link_start(anv_link_t *link, const anv_link_config_t *cfg);
int anv_link_stop(anv_link_t *link);

/* Replace the filters at run time (e.g. once the node has an ID). */
int anv_link_set_filters(anv_link_t *link, const anv_link_filter_t *filters, uint8_t n);
bool anv_link_filters_accept(const anv_link_filter_t *filters, uint8_t n, uint32_t id);

int anv_link_send(anv_link_t *link, const anv_link_frame_t *frame);
/* Next frame that passes the filters; false if none is waiting. Never blocks. */
bool anv_link_receive(anv_link_t *link, anv_link_frame_t *frame);

/* Call every loop: refreshes state and counters, handles bus-off recovery.
 * Returns ANV_LINK_EV_* flags for what changed. */
uint32_t anv_link_poll(anv_link_t *link, uint32_t now_ms);
/* Start bus-off recovery now (for callers that turned auto_recover off). */
int anv_link_recover(anv_link_t *link);

const anv_link_stats_t *anv_link_stats(const anv_link_t *link);

#ifdef __cplusplus
}
#endif

#endif /* ANV_LINK_H */
