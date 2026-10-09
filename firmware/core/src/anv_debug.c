#include "anv_debug.h"

static uint8_t clamp_level(uint8_t level)
{
    return level > ANV_DEBUG_LEVEL_MAX ? (uint8_t)ANV_DEBUG_LEVEL_MAX : level;
}

/* Level a category follows: the requested level if its mask bit is set, else the baseline. */
static uint8_t category_level(const anv_debug_t *d, uint8_t category_bit)
{
    return (d->category_mask & category_bit) != 0u ? d->level : (uint8_t)ANV_DEBUG_BASELINE_LEVEL;
}

void anv_debug_init(anv_debug_t *d, uint8_t default_level)
{
    d->default_level = clamp_level(default_level);
    d->level = d->default_level;
    d->category_mask = ANV_DEBUG_CAT_ALL;
    d->timer_active = false;
    d->expires_ms = 0u;
}

void anv_debug_set_default(anv_debug_t *d, uint8_t default_level)
{
    d->default_level = clamp_level(default_level);
}

bool anv_debug_apply(anv_debug_t *d, uint8_t level, uint8_t category_mask, uint16_t minutes, uint32_t now_ms)
{
    if (level > ANV_DEBUG_LEVEL_MAX) {
        return false;
    }
    d->level = level;
    d->category_mask = (uint8_t)(category_mask & ANV_DEBUG_CAT_ALL);
    if (minutes == 0u || level == d->default_level) {
        d->timer_active = false;
    } else {
        d->timer_active = true;
        d->expires_ms = now_ms + (uint32_t)minutes * 60000u;
    }
    return true;
}

bool anv_debug_tick(anv_debug_t *d, uint32_t now_ms)
{
    if (!d->timer_active || (int32_t)(now_ms - d->expires_ms) < 0) {
        return false;
    }
    d->timer_active = false;
    d->level = d->default_level;
    d->category_mask = ANV_DEBUG_CAT_ALL;
    return true;
}

uint8_t anv_debug_level(const anv_debug_t *d)
{
    return d->level;
}

uint16_t anv_debug_minutes_left(const anv_debug_t *d, uint32_t now_ms)
{
    if (!d->timer_active || (int32_t)(now_ms - d->expires_ms) >= 0) {
        return 0u;
    }
    uint32_t left_ms = d->expires_ms - now_ms;
    uint32_t minutes = (left_ms + 59999u) / 60000u;
    return (uint16_t)(minutes > 0xFFFFu ? 0xFFFFu : minutes);
}

bool anv_debug_event_allowed(const anv_debug_t *d, uint8_t severity)
{
    if (severity >= 2u) {
        return true;
    }
    uint8_t level = category_level(d, ANV_DEBUG_CAT_EVENTS);
    return severity == 1u ? level >= 1u : level >= 2u;
}

uint32_t anv_debug_telemetry_rate_hz(const anv_debug_t *d, uint32_t param_rate_hz)
{
    if (category_level(d, ANV_DEBUG_CAT_TELEMETRY) == 0u) {
        return ANV_DEBUG_TELEM_RATE_LEVEL0_HZ;
    }
    return param_rate_hz;
}

uint32_t anv_debug_page_rate_hz(const anv_debug_t *d)
{
    uint8_t level = category_level(d, ANV_DEBUG_CAT_PAGES);
    if (level >= 3u) {
        return ANV_DEBUG_PAGE_RATE_LEVEL3_HZ;
    }
    return level == 2u ? ANV_DEBUG_PAGE_RATE_LEVEL2_HZ : 0u;
}

uint32_t anv_rate_clamp_hb(uint32_t hz)
{
    return hz < ANV_MIN_HB_RATE_HZ ? ANV_MIN_HB_RATE_HZ : hz;
}
