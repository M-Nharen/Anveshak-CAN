/* anv_node: the generic node runtime, on top of anv_link and anv_proto.
 *
 * A node is described by its generated type table (anv_<type>_type, from schema/). The runtime
 * works only from that table, so the same code runs every node type:
 *
 *   - settings: the protocol's common parameters plus the type's, typed and range-checked,
 *     read and written over the SERVICE port, saved to flash on SAVE, some only while disarmed
 *   - streams: the type's node streams, sent at a rate a host can change at run time
 *   - commands: delivered to the application only while armed and only from the leader, each
 *     with its own watchdog; `apply: sync` commands are held until the next SYNC
 *   - actions: one-shot requests over SERVICE, passed to the application
 *
 * Built in, and not switchable: heartbeat with type ID and schema hash, automatic node ID
 * assignment (or a saved fixed ID), arm/disarm, E-stop latch, leader tracking, disarm on
 * command timeout, on leader change or loss and on fault. Outputs are off unless the node is
 * ARMED; the application's outputs_off hook is called on every way out of ARMED.
 *
 * No allocation, no RTOS, no clock of its own and nothing that blocks: the caller passes the
 * time to anv_node_poll(), every loop.
 */
#ifndef ANV_NODE_H
#define ANV_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anv_link.h"
#include "anv_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Table sizes. Override with -D if a node type needs more. */
#ifndef ANV_NODE_MAX_PARAMS
#define ANV_NODE_MAX_PARAMS 64u /* protocol parameters + the type's */
#endif
#ifndef ANV_NODE_MAX_STREAMS
#define ANV_NODE_MAX_STREAMS 16u
#endif
#ifndef ANV_NODE_MAX_COMMANDS
#define ANV_NODE_MAX_COMMANDS 8u
#endif
#define ANV_NODE_LABEL_LEN 16u /* 4 chunks of 4 characters (SERVICE label_get / label_set) */

/* ID request timing: first request after 100..600 ms, then every 400..650 ms. The jitter keeps
 * boards that power up together from asking at the same moment. */
#define ANV_NODE_ID_REQUEST_FIRST_MS 100u
#define ANV_NODE_ID_REQUEST_EVERY_MS 400u
#define ANV_NODE_ID_REQUEST_JITTER_MS 500u

typedef struct anv_node_s anv_node_t;

/* Where settings and the saved node ID live (NVS on the ESP32, RAM in tests). Keys are short
 * (at most 15 characters, NVS's limit). load returns the stored length, or 0 if the key is
 * missing or its value doesn't fit in `cap`. */
typedef struct {
    size_t (*load)(void *ctx, const char *key, void *buf, size_t cap);
    bool (*save)(void *ctx, const char *key, const void *buf, size_t len);
    bool (*erase)(void *ctx, const char *key);
} anv_node_storage_t;

/* What the node announces about itself (SERVICE info pages). */
typedef struct {
    uint32_t fw_build;
    uint32_t fw_version; /* major << 16 | minor << 8 | patch */
    uint32_t chip;       /* 1 ESP32, 2 ESP32-S3, ... (info_page `chip`) */
    uint32_t boot_count;
    uint32_t reset_reason;
} anv_node_info_t;

typedef struct {
    const anv_type_desc_t *type; /* the generated anv_<type>_type */
    anv_link_t *link;            /* started by the caller; the node sets its filters */
    uint8_t mac[6];              /* identifies the board when it asks for an ID */
    anv_node_info_t info;

    const anv_node_storage_t *storage; /* NULL: nothing is saved (SAVE reports storage_error) */
    void *storage_ctx;

    /* 0: ask the hosts for an ID (or use a saved one). 1..254: always use this ID. */
    anv_node_id_t fixed_id;
    /* Keep an assigned ID across reboots (saved like a fixed ID). Off: ask again every boot;
     * the hosts' allocator remembers the MAC, so the answer is normally the same. */
    bool save_assigned_id;

    /* --- application hooks; all optional except outputs_off. `user` is passed back. --- */
    void *user;
    /* Make every output safe NOW (motors off, valves closed). Called at start and every time the
     * node leaves ARMED, for whatever reason. Must not block. */
    void (*outputs_off)(anv_node_t *node, void *user);
    /* Once, from anv_node_begin(), after settings are loaded. */
    void (*setup)(anv_node_t *node, void *user);
    /* Every anv_node_poll(), after the runtime's own work. Never block in it. */
    void (*loop)(anv_node_t *node, uint32_t now_ms, void *user);
    /* A command from the leader while armed (for `apply: sync` commands: at the SYNC). */
    void (*on_command)(anv_node_t *node, const anv_msg_desc_t *msg, const uint8_t *data, uint8_t len, void *user);
    /* Fill one of the type's streams; return false to skip this send. `out` is zeroed. */
    bool (*fill_stream)(anv_node_t *node, const anv_msg_desc_t *msg, uint8_t out[8], void *user);
    /* A setting changed (SERVICE set, restore, the debug_level message). Read the rest in setup. */
    void (*on_setting_changed)(anv_node_t *node, const anv_param_desc_t *param, anv_value_t value, void *user);
    /* Run an action; the return value is the SERVICE status (ANV_SERVICE_STATUS_*). NULL: every
     * action reports `failed`. */
    uint8_t (*on_action)(anv_node_t *node, const anv_action_desc_t *action, anv_value_t arg, void *user);
    /* SYNC from the leader (after any held commands were delivered). */
    void (*on_sync)(anv_node_t *node, uint8_t counter, void *user);
    /* NMT identify: show yourself (blink an LED) for a few seconds. */
    void (*on_identify)(anv_node_t *node, void *user);
    /* NMT reset: restart the board. Outputs are already off. NULL: reset is ignored. */
    void (*on_reset)(anv_node_t *node, void *user);
    /* The node state changed (ANV_NODE_STATE_*), e.g. for a status LED. */
    void (*on_state_changed)(anv_node_t *node, uint8_t old_state, uint8_t new_state, void *user);
    /* Escape hatch: every received frame the runtime didn't handle itself (other nodes' streams,
     * protocol ports it doesn't implement, ...). The built-in safety handling always runs first. */
    void (*on_frame)(anv_node_t *node, const anv_link_frame_t *frame, void *user);
} anv_node_config_t;

typedef struct {
    uint32_t tx_dropped;       /* frames the link refused */
    uint32_t commands;         /* delivered to on_command */
    uint32_t commands_ignored; /* not from the leader, not armed, or too short */
    uint32_t watchdog_trips;
    uint32_t leader_changes;
    uint32_t leader_losses;
    uint32_t estops;
    uint32_t services;
} anv_node_stats_t;

typedef struct {
    const anv_msg_desc_t *msg;
    uint32_t rate_mhz;
    uint32_t period_ms;
    uint32_t next_ms;
} anv_node_stream_t;

typedef struct {
    const anv_msg_desc_t *msg;
    uint32_t last_ms;    /* last accepted while armed (watchdog) */
    bool held;           /* `apply: sync`: waiting for the next SYNC */
    uint8_t held_len;
    uint8_t held_data[8];
} anv_node_command_t;

struct anv_node_s {
    anv_node_config_t cfg;
    anv_node_stats_t stats;

    uint8_t state;     /* anv_node_state_t */
    anv_node_id_t id;  /* ANV_NODE_UNASSIGNED until known */
    bool id_saved;
    bool estop_latched;
    bool fault_active; /* the application's fault condition */
    bool fault_latched;

    /* settings: protocol parameters first, then the type's */
    const anv_param_desc_t *params[ANV_NODE_MAX_PARAMS];
    anv_value_t values[ANV_NODE_MAX_PARAMS];
    uint16_t n_params;
    bool unsaved;
    char label[ANV_NODE_LABEL_LEN];

    anv_node_stream_t streams[ANV_NODE_MAX_STREAMS];
    uint8_t n_streams;
    anv_node_command_t commands[ANV_NODE_MAX_COMMANDS];
    uint8_t n_commands;

    /* leader tracking */
    bool have_leader;
    anv_node_id_t leader;
    uint16_t leader_term;
    uint32_t leader_seen_ms;

    uint32_t start_ms;
    uint32_t now_ms;
    uint32_t heartbeat_next_ms;
    uint32_t id_request_next_ms;
    bool debug_timed;        /* the debug_level message set a timeout */
    uint32_t debug_until_ms;
    uint32_t rng;
    bool begun;
};

/* Fill a config with defaults: no hooks, ask for an ID, don't keep it. */
void anv_node_config_default(anv_node_config_t *cfg);

/* Check the config, build the tables from the type, load saved settings and ID.
 * False if the type, link or outputs_off is missing or the type doesn't fit the tables. */
bool anv_node_init(anv_node_t *node, const anv_node_config_t *cfg);
/* Set the link filters, turn outputs off, call setup. Call once the link is started. */
void anv_node_begin(anv_node_t *node, uint32_t now_ms);
/* Call every loop: polls the link (bus-off recovery), handles received frames, timers, streams
 * and the heartbeat, then calls the loop hook. */
void anv_node_poll(anv_node_t *node, uint32_t now_ms);

/* --- state ------------------------------------------------------------------------------- */
uint8_t anv_node_state(const anv_node_t *node);   /* ANV_NODE_STATE_* */
bool anv_node_armed(const anv_node_t *node);      /* the only state in which outputs may be on */
anv_node_id_t anv_node_id(const anv_node_t *node); /* ANV_NODE_UNASSIGNED while asking */
bool anv_node_has_leader(const anv_node_t *node);
anv_node_id_t anv_node_leader(const anv_node_t *node);
const anv_node_stats_t *anv_node_stats(const anv_node_t *node);
const char *anv_node_state_name(uint8_t state);

/* Latch E-stop here (local E-stop input) and tell every other node. */
void anv_node_estop(anv_node_t *node, uint8_t reason);
/* The application's fault condition. Active: disarm and go to FAULT. FAULT is left only by an
 * NMT fault_reset from the leader once the condition is gone. */
void anv_node_set_fault(anv_node_t *node, bool active);
/* Disarm now (the application's own reason). */
void anv_node_disarm(anv_node_t *node);

/* --- node ID ----------------------------------------------------------------------------- */
/* Save `id` (1..254) as this board's fixed ID and use it now. Refused while armed. */
bool anv_node_save_id(anv_node_t *node, anv_node_id_t id);
/* Erase the saved ID and ask the hosts again. Refused while armed. */
bool anv_node_forget_id(anv_node_t *node);

/* --- settings ---------------------------------------------------------------------------- */
const anv_param_desc_t *anv_node_param(const anv_node_t *node, uint16_t index);
/* Current value; false if there is no such parameter. */
bool anv_node_get(const anv_node_t *node, uint16_t index, anv_value_t *out);
/* The application's own write: type, range and disarmed-only checks as for a SERVICE set, but
 * read-only (`access: ro`) parameters may be written, since the node is what reports them.
 * Returns a SERVICE status. */
uint8_t anv_node_set(anv_node_t *node, uint16_t index, anv_value_t value);
uint8_t anv_node_save(anv_node_t *node);
uint8_t anv_node_restore_defaults(anv_node_t *node);
const char *anv_node_label(const anv_node_t *node);

/* --- streams ----------------------------------------------------------------------------- */
uint32_t anv_node_stream_rate(const anv_node_t *node, uint16_t port); /* mHz, 0 = off */
uint8_t anv_node_set_stream_rate(anv_node_t *node, uint16_t port, uint32_t rate_mhz);
/* Send one of this type's messages now (an event, or a stream out of turn). */
bool anv_node_send(anv_node_t *node, uint16_t port, const uint8_t *data, uint8_t len);

/* --- raw frames -------------------------------------------------------------------------- */
/* Escape hatch: send any frame from this node (src is forced to this node's ID). */
bool anv_node_send_frame(anv_node_t *node, anv_link_frame_t *frame);

/* --- storage for tests and hosts: a small key/value store in RAM ------------------------- */
#define ANV_NODE_MEMSTORE_SLOTS 4u
#define ANV_NODE_MEMSTORE_SIZE 1024u
typedef struct {
    char key[16];
    size_t len;
    uint8_t data[ANV_NODE_MEMSTORE_SIZE];
    bool used;
} anv_node_memstore_slot_t;
typedef struct {
    anv_node_memstore_slot_t slots[ANV_NODE_MEMSTORE_SLOTS];
    bool fail_writes; /* test hook */
} anv_node_memstore_t;
void anv_node_memstore_init(anv_node_memstore_t *store);
const anv_node_storage_t *anv_node_memstore_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ANV_NODE_H */
