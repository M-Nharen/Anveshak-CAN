#include "anv_cmdwatch.h"

void anv_cmd_watchdog_init(anv_cmd_watchdog_t *watchdog)
{
    watchdog->have_cmd = false;
    watchdog->last_cmd_ms = 0;
    watchdog->have_seq = false;
    watchdog->last_seq = 0;
    watchdog->last_seq_change_ms = 0;
}

void anv_cmd_watchdog_on_command(anv_cmd_watchdog_t *watchdog, uint8_t seq, uint32_t now_ms)
{
    watchdog->have_cmd = true;
    watchdog->last_cmd_ms = now_ms;

    if (!watchdog->have_seq || watchdog->last_seq != seq) {
        watchdog->have_seq = true;
        watchdog->last_seq = seq;
        watchdog->last_seq_change_ms = now_ms;
    }
}

bool anv_cmd_watchdog_is_fresh(const anv_cmd_watchdog_t *watchdog, uint32_t now_ms)
{
    if (!watchdog->have_cmd || (now_ms - watchdog->last_cmd_ms) >= ANV_CMD_WATCHDOG_MS) {
        return false;
    }
    if (!watchdog->have_seq || (now_ms - watchdog->last_seq_change_ms) >= ANV_CMD_WATCHDOG_MS) {
        return false;
    }
    return true;
}

void anv_cmd_watchdog_reset(anv_cmd_watchdog_t *watchdog)
{
    anv_cmd_watchdog_init(watchdog);
}
