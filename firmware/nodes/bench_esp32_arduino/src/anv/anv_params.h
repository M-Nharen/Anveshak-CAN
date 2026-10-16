/* Generic parameter store: works against any node type's parameter table (defs/params, a
 * YAML file per node type, generated to e.g. firmware/generated/params_drive.h). The core
 * only needs each entry's
 * index, type, range and whether it is safety-related (disarmed_only); the application
 * builds an anv_param_descriptor_t array from its generated ROVER_PARAMS_<TYPE>_TABLE once
 * at startup, so core never depends on a specific node type's generated header. */
#ifndef ANV_PARAMS_H
#define ANV_PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anv_board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANV_PARAM_TYPE_F32,
    ANV_PARAM_TYPE_I32,
    ANV_PARAM_TYPE_U32,
    ANV_PARAM_TYPE_U8,
} anv_param_type_t;

typedef union {
    float f32;
    int32_t i32;
    uint32_t u32;
} anv_param_value_t;

typedef struct {
    uint16_t index;
    anv_param_type_t type;
    double minimum;
    double maximum;
    double default_value;
    bool disarmed_only;
} anv_param_descriptor_t;

/* Matches the STATUS signal used in SERVICE responses. */
typedef enum {
    ANV_PARAM_STATUS_OK = 0,
    ANV_PARAM_STATUS_BAD_INDEX = 1,
    ANV_PARAM_STATUS_OUT_OF_RANGE = 2,
    ANV_PARAM_STATUS_NOT_ALLOWED_WHILE_ARMED = 3,
    ANV_PARAM_STATUS_FLASH_ERROR = 4,
} anv_param_status_t;

typedef struct {
    anv_param_value_t *values;                  /* caller-owned, length == count */
    const anv_param_descriptor_t *descriptors;  /* caller-owned, length == count */
    size_t count;
    const anv_board_hooks_t *board;             /* may be NULL: SAVE then reports FLASH_ERROR */
    bool dirty;                                  /* unsaved changes since last SAVE */
} anv_param_store_t;

void anv_param_store_init(anv_param_store_t *store, anv_param_value_t *values,
                           const anv_param_descriptor_t *descriptors, size_t count,
                           const anv_board_hooks_t *board);

/* Sets every value to its descriptor's default. Does not touch `dirty` or persistent storage. */
void anv_param_store_load_defaults(anv_param_store_t *store);

/* Loads saved values from board storage if present, otherwise falls back to defaults. */
void anv_param_store_load(anv_param_store_t *store);

anv_param_status_t anv_param_get(const anv_param_store_t *store, uint16_t index, anv_param_value_t *out);

/* `armed` is the node's current ARMED state; SET on a disarmed_only parameter is refused
 * while armed. */
anv_param_status_t anv_param_set(anv_param_store_t *store, uint16_t index, anv_param_value_t value, bool armed);

anv_param_status_t anv_param_save(anv_param_store_t *store);

/* disarmed_only parameters are only restored while disarmed, same as SET. */
anv_param_status_t anv_param_restore_defaults(anv_param_store_t *store, bool armed);

bool anv_param_store_dirty(const anv_param_store_t *store);

#ifdef __cplusplus
}
#endif

#endif /* ANV_PARAMS_H */
