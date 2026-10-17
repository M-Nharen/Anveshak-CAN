#include "anv_link_twai_filter.h"

static uint32_t dual_half(uint32_t v)
{
    return ((v & ANV_LINK_EXT_ID_MASK) >> ANV_TWAI_DUAL_SHIFT) & 0xFFFFu;
}

anv_twai_hw_filter_t anv_twai_hw_filter(const anv_link_filter_t *filters, uint8_t n)
{
    anv_twai_hw_filter_t hw = {false, false, 0u, 0u};
    if (n == 0u) {
        return hw;
    }
    hw.enabled = true;
    if (n == 1u) {
        hw.mask = filters[0].mask & ANV_LINK_EXT_ID_MASK;
        hw.id = filters[0].id & hw.mask;
        return hw;
    }
    if (n == 2u) {
        /* Filter 1 sits in the high half, filter 2 in the low half (twai_make_dual_filter). */
        hw.dual = true;
        hw.id = (dual_half(filters[0].id & filters[0].mask) << 16) | dual_half(filters[1].id & filters[1].mask);
        hw.mask = (dual_half(filters[0].mask) << 16) | dual_half(filters[1].mask);
        return hw;
    }
    /* More than two: one pair covering all of them. Keep only the bits every filter cares
     * about and on which they all agree. */
    uint32_t mask = ANV_LINK_EXT_ID_MASK;
    for (uint8_t i = 0; i < n; i++) {
        mask &= filters[i].mask;
    }
    for (uint8_t i = 1; i < n; i++) {
        mask &= ~(filters[i].id ^ filters[0].id);
    }
    hw.mask = mask;
    hw.id = filters[0].id & mask;
    return hw;
}

bool anv_twai_hw_accepts(const anv_twai_hw_filter_t *hw, uint32_t id)
{
    if (!hw->enabled) {
        return true;
    }
    if (!hw->dual) {
        return ((id ^ hw->id) & hw->mask) == 0u;
    }
    const uint32_t top = dual_half(id);
    const bool first = ((top ^ (hw->id >> 16)) & (hw->mask >> 16)) == 0u;
    const bool second = ((top ^ (hw->id & 0xFFFFu)) & (hw->mask & 0xFFFFu)) == 0u;
    return first || second;
}
