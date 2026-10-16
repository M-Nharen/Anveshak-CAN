/* Command watchdog: 100 ms since the last accepted command from the current master, per
 * docs/architecture.md ("Watchdogs: three layers", layer 1). Also catches a host stuck
 * re-sending its last command: a sequence counter that stops changing counts as a lost
 * master even though frames keep arriving. */
#ifndef ANV_CMDWATCH_H
#define ANV_CMDWATCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_CMD_WATCHDOG_MS 100u

typedef struct {
    bool have_cmd;
    uint32_t last_cmd_ms;
    bool have_seq;
    uint8_t last_seq;
    uint32_t last_seq_change_ms;
} anv_cmd_watchdog_t;

void anv_cmd_watchdog_init(anv_cmd_watchdog_t *watchdog);

/* Call once for every command frame accepted as coming from the current master (sender and
 * term already checked by the caller via anv_master_check). `seq` is the command's sequence
 * counter. */
void anv_cmd_watchdog_on_command(anv_cmd_watchdog_t *watchdog, uint8_t seq, uint32_t now_ms);

/* Call every tick. True if a command arrived within the last ANV_CMD_WATCHDOG_MS *and* its
 * sequence counter has changed within that same window -- i.e. the master is both present
 * and actually driving, not stuck re-sending one frame. */
bool anv_cmd_watchdog_is_fresh(const anv_cmd_watchdog_t *watchdog, uint32_t now_ms);

void anv_cmd_watchdog_reset(anv_cmd_watchdog_t *watchdog);

#ifdef __cplusplus
}
#endif

#endif /* ANV_CMDWATCH_H */
