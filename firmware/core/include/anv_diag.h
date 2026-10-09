/* Node-side error reporting and the black box (docs/architecture.md, "Error reporting and
 * diagnostics").
 *
 * Events are what a node tells the bus about itself (NODE_EVENT):
 *   - report()  a condition became true. Edge-triggered: reporting it again while it is still
 *               active sends nothing. clear() sends the matching "cleared" event.
 *   - event()   something happened once (a new master, a bad frame); no cleared event.
 *   Each error code is limited to one frame per second; events inside that second are folded
 *   into one frame that goes out when the second is over (latest detail wins). The sequence
 *   number counts only frames that were really sent, so a host seeing a gap knows it lost one.
 *
 * Black box: every event goes into a RAM ring (64). Errors and criticals are also kept in a
 * flash log (the last 32) together with a boot count, so they survive a reset. After a reset the
 * flash entries are loaded back into the ring, so LOG_READ shows "what happened before the
 * crash" first. Flash is written rarely (flash wear): at boot, on a critical event, and at most
 * once per ANV_DIAG_FLASH_MIN_INTERVAL_MS otherwise -- and never from the periodic path while
 * the node is armed (hold_flash), because a flash erase can stall the CPU for a second or more
 * and the 100 ms command watchdog must not be starved while driving.
 *
 * Severity comes from the generated error registry (errors.h) so a code can't be reported at
 * the wrong severity. */
#ifndef ANV_DIAG_H
#define ANV_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anv_board.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_LOG_RAM_ENTRIES 64u
#define ANV_LOG_FLASH_ENTRIES 32u
#define ANV_DIAG_SLOTS 16u
#define ANV_DIAG_MIN_INTERVAL_MS 1000u
#define ANV_DIAG_FLASH_MIN_INTERVAL_MS 5000u

/* Serialised size of the flash blob handed to the board's log_save()/log_load(). Boards size
 * their reserved page / NVS blob with this. */
#define ANV_DIAG_BLOB_SIZE 400u

/* flags */
#define ANV_LOG_FLAG_CLEARED 0x01u
#define ANV_LOG_FLAG_NOT_SENT 0x02u /* kept in the log only: debug level hid it */

typedef struct {
    uint16_t code;
    uint8_t severity;
    uint8_t flags;
    uint16_t detail;
    uint16_t uptime_ds; /* 0.1 s since boot, wraps after ~1.8 h */
    uint16_t boot_count;
    uint8_t seq;
    uint8_t repeat; /* events folded into this one by the rate limiter */
} anv_log_entry_t;

typedef struct {
    uint8_t severity;
    bool cleared;
    uint16_t code;
    uint8_t seq;
    uint16_t detail;
    uint16_t uptime_ds;
} anv_diag_msg_t;

/* Puts the event on the bus. Returns false if it could not (queue full): the diag layer then
 * keeps it pending and tries again on the next tick. */
typedef bool (*anv_diag_sink_t)(void *ctx, const anv_diag_msg_t *msg);

/* Debug-level gate: true if an event of this severity may be put on the bus. NULL = always. */
typedef bool (*anv_diag_gate_t)(void *ctx, uint8_t severity);

typedef struct {
    uint16_t code;
    uint8_t severity;
    bool used;
    bool active;
    bool sent_once;
    bool pending;
    bool pending_cleared;
    uint16_t pending_detail;
    uint8_t coalesced;
    uint32_t last_sent_ms;
} anv_diag_slot_t;

typedef struct {
    const anv_board_hooks_t *board;
    anv_diag_sink_t sink;
    anv_diag_gate_t gate;
    void *sink_ctx;

    uint32_t boot_ms;
    uint16_t boot_count;
    uint8_t seq;

    anv_diag_slot_t slots[ANV_DIAG_SLOTS];

    anv_log_entry_t ram[ANV_LOG_RAM_ENTRIES];
    uint16_t ram_head;  /* next write position */
    uint16_t ram_count;

    anv_log_entry_t flash[ANV_LOG_FLASH_ENTRIES];
    uint16_t flash_head;
    uint16_t flash_count;
    bool flash_dirty;
    uint32_t last_flash_write_ms;
    bool flash_error;
    bool hold_flash; /* set while armed: no periodic flash writes (an erase can stall the CPU) */
} anv_diag_t;

/* Loads the flash log (if the board has one and the CRC is good), bumps and saves the boot
 * count, and preloads the RAM ring with what was in flash. */
void anv_diag_init(anv_diag_t *d, const anv_board_hooks_t *board, uint32_t now_ms);
void anv_diag_set_sink(anv_diag_t *d, anv_diag_sink_t sink, anv_diag_gate_t gate, void *ctx);

/* Condition became true (edge). `code` must be in the generated registry; unknown codes are
 * ignored. */
void anv_diag_report(anv_diag_t *d, uint16_t code, uint16_t detail, uint32_t now_ms);
/* Condition went away: sends the cleared event if it was active. */
void anv_diag_clear(anv_diag_t *d, uint16_t code, uint16_t detail, uint32_t now_ms);
/* One-shot event. */
void anv_diag_event(anv_diag_t *d, uint16_t code, uint16_t detail, uint32_t now_ms);
bool anv_diag_is_active(const anv_diag_t *d, uint16_t code);

/* Sends events the rate limiter held back and writes the flash log if it is due. */
void anv_diag_tick(anv_diag_t *d, uint32_t now_ms);

/* Writes the flash log now (e.g. before a deliberate reset). Returns false on failure. */
bool anv_diag_flush(anv_diag_t *d, uint32_t now_ms);

/* Black box access. index 0 = newest. False if index >= count. */
uint16_t anv_diag_log_count(const anv_diag_t *d);
bool anv_diag_log_get(const anv_diag_t *d, uint16_t index, anv_log_entry_t *out);
/* LOG_CLEAR: wipes RAM and flash log (the boot count is kept). */
void anv_diag_log_clear(anv_diag_t *d, uint32_t now_ms);

/* Registry lookup: true and the severity if `code` is a known error code. */
bool anv_diag_severity_of(uint16_t code, uint8_t *severity);

/* Exposed for tests of the flash format. */
size_t anv_diag_serialize(const anv_diag_t *d, uint8_t *out, size_t cap);
bool anv_diag_deserialize(anv_diag_t *d, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ANV_DIAG_H */
