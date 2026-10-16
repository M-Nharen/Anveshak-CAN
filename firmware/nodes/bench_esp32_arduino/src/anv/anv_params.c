#include "anv_params.h"

#include <stddef.h>

static const anv_param_descriptor_t *find_descriptor(const anv_param_store_t *store, uint16_t index,
                                                       size_t *slot_out)
{
    for (size_t i = 0; i < store->count; i++) {
        if (store->descriptors[i].index == index) {
            if (slot_out != NULL) {
                *slot_out = i;
            }
            return &store->descriptors[i];
        }
    }
    return NULL;
}

static double value_as_double(anv_param_type_t type, anv_param_value_t value)
{
    switch (type) {
    case ANV_PARAM_TYPE_F32:
        return (double)value.f32;
    case ANV_PARAM_TYPE_I32:
        return (double)value.i32;
    case ANV_PARAM_TYPE_U32:
    case ANV_PARAM_TYPE_U8:
    default:
        return (double)value.u32;
    }
}

static anv_param_value_t double_as_value(anv_param_type_t type, double v)
{
    anv_param_value_t out;
    switch (type) {
    case ANV_PARAM_TYPE_F32:
        out.f32 = (float)v;
        break;
    case ANV_PARAM_TYPE_I32:
        out.i32 = (int32_t)v;
        break;
    case ANV_PARAM_TYPE_U32:
    case ANV_PARAM_TYPE_U8:
    default:
        out.u32 = (uint32_t)v;
        break;
    }
    return out;
}

void anv_param_store_init(anv_param_store_t *store, anv_param_value_t *values,
                           const anv_param_descriptor_t *descriptors, size_t count,
                           const anv_board_hooks_t *board)
{
    store->values = values;
    store->descriptors = descriptors;
    store->count = count;
    store->board = board;
    store->dirty = false;
}

void anv_param_store_load_defaults(anv_param_store_t *store)
{
    for (size_t i = 0; i < store->count; i++) {
        store->values[i] = double_as_value(store->descriptors[i].type, store->descriptors[i].default_value);
    }
}

void anv_param_store_load(anv_param_store_t *store)
{
    anv_param_store_load_defaults(store);
    if (store->board != NULL && store->board->storage_load != NULL) {
        (void)store->board->storage_load(store->board->ctx, (uint8_t *)store->values,
                                          store->count * sizeof(anv_param_value_t));
    }
    store->dirty = false;
}

anv_param_status_t anv_param_get(const anv_param_store_t *store, uint16_t index, anv_param_value_t *out)
{
    size_t slot;
    if (find_descriptor(store, index, &slot) == NULL) {
        return ANV_PARAM_STATUS_BAD_INDEX;
    }
    *out = store->values[slot];
    return ANV_PARAM_STATUS_OK;
}

anv_param_status_t anv_param_set(anv_param_store_t *store, uint16_t index, anv_param_value_t value, bool armed)
{
    size_t slot;
    const anv_param_descriptor_t *desc = find_descriptor(store, index, &slot);
    if (desc == NULL) {
        return ANV_PARAM_STATUS_BAD_INDEX;
    }
    if (desc->disarmed_only && armed) {
        return ANV_PARAM_STATUS_NOT_ALLOWED_WHILE_ARMED;
    }
    double v = value_as_double(desc->type, value);
    if (v < desc->minimum || v > desc->maximum) {
        return ANV_PARAM_STATUS_OUT_OF_RANGE;
    }
    store->values[slot] = value;
    store->dirty = true;
    return ANV_PARAM_STATUS_OK;
}

anv_param_status_t anv_param_save(anv_param_store_t *store)
{
    if (store->board == NULL || store->board->storage_save == NULL) {
        return ANV_PARAM_STATUS_FLASH_ERROR;
    }
    int rc = store->board->storage_save(store->board->ctx, (const uint8_t *)store->values,
                                         store->count * sizeof(anv_param_value_t));
    if (rc != 0) {
        return ANV_PARAM_STATUS_FLASH_ERROR;
    }
    store->dirty = false;
    return ANV_PARAM_STATUS_OK;
}

anv_param_status_t anv_param_restore_defaults(anv_param_store_t *store, bool armed)
{
    for (size_t i = 0; i < store->count; i++) {
        if (store->descriptors[i].disarmed_only && armed) {
            return ANV_PARAM_STATUS_NOT_ALLOWED_WHILE_ARMED;
        }
    }
    anv_param_store_load_defaults(store);
    store->dirty = true;
    return ANV_PARAM_STATUS_OK;
}

bool anv_param_store_dirty(const anv_param_store_t *store)
{
    return store->dirty;
}
