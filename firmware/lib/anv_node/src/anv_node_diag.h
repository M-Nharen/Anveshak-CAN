/* anv_node diagnostics: events (protocol or the node's own type), a small black box of the
 * last ANV_NODE_DIAG_LOG_CAP events that survives reboots (via cfg.storage), and the log
 * transfer (schema's transfer_service `log`) that lets a host read it back.
 *
 * Debug levels themselves (the debug_level message and parameter) are handled directly by
 * anv_node; this file is only events and the black box.
 */
#ifndef ANV_NODE_DIAG_H
#define ANV_NODE_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anv_node_s anv_node_t; /* defined in anv_node.h */

#define ANV_NODE_DIAG_LOG_CAP 24u
#define ANV_NODE_DIAG_RATE_SLOTS 8u

typedef struct {
    uint16_t code;
    uint16_t detail;
    uint16_t uptime_s;
    uint8_t severity; /* anv_severity_t */
    bool cleared;
    uint8_t seq;
} anv_diag_entry_t;

typedef struct {
    anv_diag_entry_t entries[ANV_NODE_DIAG_LOG_CAP];
    uint8_t count;     /* entries used, growing to the cap */
    uint8_t next;      /* where the next entry goes (ring) */
    uint8_t seq;       /* increments with every event raised */

    /* per-code rate limiting: last ms an event of this code was let through */
    uint16_t rate_code[ANV_NODE_DIAG_RATE_SLOTS];
    uint32_t rate_ms[ANV_NODE_DIAG_RATE_SLOTS];
    uint8_t rate_next; /* next slot to evict */

    /* anv_isotp_send() keeps a pointer to what it's given, not a copy ("the data passed to
     * anv_isotp_send() must stay valid until on_tx_done runs", anv_isotp.h): the log reply has
     * to be built into somewhere that outlives the anv_node_i_diag_handle() call that starts
     * sending it, not a local on its stack. */
    uint8_t tx_buf[2u + (size_t)ANV_NODE_DIAG_LOG_CAP * 8u];
} anv_node_diag_t;

/* Raise an event: broadcasts it (protocol.yaml's `event` message), appends it to the black box
 * and saves the box (if cfg.storage is set), unless the same code was raised within
 * 1000 / event_rate_limit ms, in which case it's dropped entirely (bus and flash wear both). */
void anv_node_event(anv_node_t *node, uint16_t code, uint16_t detail);
/* Like anv_node_event, but with the `cleared` bit set (the condition ended). Never rate-limited. */
void anv_node_event_clear(anv_node_t *node, uint16_t code, uint16_t detail);

uint8_t anv_node_diag_log_count(const anv_node_t *node);
/* Entries oldest first, as kept (index 0 is the oldest still in the box). */
const anv_diag_entry_t *anv_node_diag_log_entry(const anv_node_t *node, uint8_t i);

#ifdef __cplusplus
}
#endif

#endif /* ANV_NODE_DIAG_H */
