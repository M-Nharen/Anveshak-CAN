/* Debug levels (docs/architecture.md, "Debug levels: switching debugging on and off").
 *
 * Debugging is off unless turned on. A node always sends its heartbeat, feedback, and NODE_EVENT
 * for errors and criticals; the level only adds or removes the rest:
 *
 *   level 0 Off    telemetry slowed to 1 Hz, no warnings or info events on the bus
 *   level 1 Normal warnings, telemetry at its parameter rate (the baseline)
 *   level 2 Debug  + info events, + debug pages (DRIVE_DEBUG_A/B) at 10 Hz
 *   level 3 Trace  + debug pages at 50 Hz (the host keeps this to one node at a time)
 *
 * The category mask (DEBUG_LEVEL byte 2) lets a request apply to some parts only: bit 0 events,
 * bit 1 telemetry, bit 2 debug pages, bit 3 error counts by type. A category whose bit is clear
 * stays at the baseline (level 1) instead of following the requested level, so "debug pages
 * only" is level 2 with mask 0x04.
 *
 * A request can carry an auto-off timer in minutes; when it expires the node goes back to its
 * default level (the `debug_default` parameter), so a forgotten session never lasts.
 */
#ifndef ANV_DEBUG_H
#define ANV_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_DEBUG_LEVEL_MAX 3u
#define ANV_DEBUG_BASELINE_LEVEL 1u

#define ANV_DEBUG_CAT_EVENTS (1u << 0)
#define ANV_DEBUG_CAT_TELEMETRY (1u << 1)
#define ANV_DEBUG_CAT_PAGES (1u << 2)
#define ANV_DEBUG_CAT_ERRCOUNTS (1u << 3)
#define ANV_DEBUG_CAT_ALL 0x0Fu

#define ANV_DEBUG_PAGE_RATE_LEVEL2_HZ 10u
#define ANV_DEBUG_PAGE_RATE_LEVEL3_HZ 50u
#define ANV_DEBUG_TELEM_RATE_LEVEL0_HZ 1u

/* Safety floor: a node heartbeat slower than this would make a healthy node look missing. */
#define ANV_MIN_HB_RATE_HZ 5u

typedef struct {
    uint8_t level;
    uint8_t default_level;
    uint8_t category_mask;
    bool timer_active;
    uint32_t expires_ms;
} anv_debug_t;

/* Starts at `default_level` (clamped to 0-3) with every category enabled. */
void anv_debug_init(anv_debug_t *d, uint8_t default_level);

/* The level a reboot or an expired timer returns to; follows the debug_default parameter. */
void anv_debug_set_default(anv_debug_t *d, uint8_t default_level);

/* Applies a DEBUG_LEVEL request. `minutes` == 0 means "until changed". False (nothing changed)
 * for a level above 3. A request equal to the default level with no timer simply clears any
 * running timer. */
bool anv_debug_apply(anv_debug_t *d, uint8_t level, uint8_t category_mask, uint16_t minutes, uint32_t now_ms);

/* Returns true on the call where an auto-off timer expired and the level went back to default. */
bool anv_debug_tick(anv_debug_t *d, uint32_t now_ms);

/* Level reported in NODE_HB. */
uint8_t anv_debug_level(const anv_debug_t *d);

/* Minutes left on the auto-off timer (rounded up), 0 if none. */
uint16_t anv_debug_minutes_left(const anv_debug_t *d, uint32_t now_ms);

/* Whether NODE_EVENT of this severity (0 info .. 3 critical) is put on the bus. Errors and
 * criticals always are; warnings need level >= 1, info needs level >= 2 (for the events
 * category). */
bool anv_debug_event_allowed(const anv_debug_t *d, uint8_t severity);

/* Telemetry rate: the parameter value, except 1 Hz at level 0 (for the telemetry category). */
uint32_t anv_debug_telemetry_rate_hz(const anv_debug_t *d, uint32_t param_rate_hz);

/* DRIVE_DEBUG_A/B rate: 0 (off) below level 2, 10 Hz at level 2, 50 Hz at level 3. */
uint32_t anv_debug_page_rate_hz(const anv_debug_t *d);

/* Applies the safety floor to the heartbeat rate parameter. */
uint32_t anv_rate_clamp_hb(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif /* ANV_DEBUG_H */
