/* anv_schema.h: types shared by all generated schema code. DO NOT EDIT; generated. */
#ifndef ANV_SCHEMA_H
#define ANV_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Names in the descriptor tables cost flash; define ANV_SCHEMA_NO_NAMES to drop them. */
#ifdef ANV_SCHEMA_NO_NAMES
#define ANV_NAME(s) NULL
#else
#define ANV_NAME(s) (s)
#endif

/* Parameter / action argument value types (what a 32-bit service value means). */
typedef enum {
    ANV_VT_NONE = 0,
    ANV_VT_BOOL,
    ANV_VT_U8,
    ANV_VT_I8,
    ANV_VT_U16,
    ANV_VT_I16,
    ANV_VT_U32,
    ANV_VT_I32,
    ANV_VT_F32
} anv_value_type_t;

/* One parameter value, as carried in a SERVICE frame's 32-bit value field. */
typedef union {
    uint32_t u;
    int32_t i;
    float f;
} anv_value_t;

typedef enum { ANV_KIND_STREAM, ANV_KIND_EVENT, ANV_KIND_COMMAND, ANV_KIND_FRAME } anv_kind_t;
typedef enum { ANV_SENDER_NODE, ANV_SENDER_HOST, ANV_SENDER_LEADER, ANV_SENDER_ANY } anv_sender_t;

#define ANV_MSG_APPLY_SYNC 0x01u   /* command is held and applied at the next SYNC */

typedef struct {
    uint16_t port;
    uint8_t priority;
    uint8_t kind;               /* anv_kind_t */
    uint8_t sender;             /* anv_sender_t */
    uint8_t length;             /* bytes */
    uint8_t flags;              /* ANV_MSG_* */
    uint16_t watchdog_ms;       /* commands: 0 = no watchdog */
    uint32_t rate_default_mhz;  /* streams: rates in millihertz (0 = off) */
    uint32_t rate_min_mhz;
    uint32_t rate_max_mhz;
    const char *name;
} anv_msg_desc_t;

#define ANV_PARAM_READ_ONLY     0x01u
#define ANV_PARAM_PERSIST       0x02u
#define ANV_PARAM_DISARMED_ONLY 0x04u
#define ANV_PARAM_REBOOT        0x08u
#define ANV_PARAM_HAS_MIN       0x10u
#define ANV_PARAM_HAS_MAX       0x20u

typedef struct {
    uint16_t index;
    uint8_t type;               /* anv_value_type_t */
    uint8_t flags;              /* ANV_PARAM_* */
    anv_value_t def;
    anv_value_t min;
    anv_value_t max;
    const char *name;
    const char *unit;
} anv_param_desc_t;

#define ANV_ACTION_DISARMED_ONLY 0x01u

typedef struct {
    uint16_t index;
    uint8_t arg_type;           /* anv_value_type_t, ANV_VT_NONE = no argument */
    uint8_t flags;              /* ANV_ACTION_* */
    const char *name;
} anv_action_desc_t;

typedef struct {
    uint16_t code;
    uint8_t severity;           /* 0 info, 1 warning, 2 error, 3 critical */
    const char *name;
} anv_event_desc_t;

/* Everything a node of one type can do. The node library works only from this. */
typedef struct {
    const char *name;
    uint16_t type_id;
    uint16_t version;
    uint32_t wire_hash;
    const anv_msg_desc_t *messages;
    uint16_t n_messages;
    const anv_param_desc_t *params;
    uint16_t n_params;
    const anv_action_desc_t *actions;
    uint16_t n_actions;
    const anv_event_desc_t *events;
    uint16_t n_events;
} anv_type_desc_t;

/* --- bit helpers used by the generated pack/unpack code ------------------------------ */
/* A frame is one little-endian 64-bit integer; a field of n bits at bit s is
 * (frame >> s) & mask(n). Same rule as DBC "Intel" byte order. */

static inline uint64_t anv_bits_load(const uint8_t *data, uint8_t len)
{
    uint64_t v = 0;
    for (uint8_t i = 0; i < len && i < 8; i++) {
        v |= (uint64_t)data[i] << (8u * i);
    }
    return v;
}

static inline void anv_bits_store(uint64_t v, uint8_t *out, uint8_t len)
{
    for (uint8_t i = 0; i < len && i < 8; i++) {
        out[i] = (uint8_t)(v >> (8u * i));
    }
}

static inline uint64_t anv_bits_mask(uint8_t bits)
{
    return bits >= 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1u);
}

static inline uint64_t anv_bits_get(uint64_t frame, uint8_t start, uint8_t bits)
{
    return (frame >> start) & anv_bits_mask(bits);
}

static inline uint64_t anv_bits_put(uint64_t value, uint8_t start, uint8_t bits)
{
    return (value & anv_bits_mask(bits)) << start;
}

static inline int64_t anv_bits_signed(uint64_t raw, uint8_t bits)
{
    if (bits >= 64) {
        return (int64_t)raw;
    }
    uint64_t sign = (uint64_t)1 << (bits - 1);
    return (int64_t)((raw ^ sign) - sign);
}

static inline uint32_t anv_f32_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static inline float anv_bits_f32(uint64_t raw)
{
    uint32_t u = (uint32_t)raw;
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* Physical values in the generated *_to_phys / *_from_phys helpers. float by default: the
 * ESP32's FPU is single precision and double is done in software. Define
 * ANV_SCHEMA_PHYS_DOUBLE for double (e.g. a 32-bit field with a fine scale). */
#ifdef ANV_SCHEMA_PHYS_DOUBLE
typedef double anv_phys_t;
#else
typedef float anv_phys_t;
#endif

/* Round a physical value to the nearest raw integer and clamp it to [lo, hi]. */
static inline int64_t anv_round_clamp(anv_phys_t v, int64_t lo, int64_t hi)
{
    if (v != v) {
        return 0; /* NaN */
    }
    if (v <= (anv_phys_t)lo) {
        return lo;
    }
    if (v >= (anv_phys_t)hi) {
        return hi;
    }
    return (int64_t)(v >= 0 ? v + (anv_phys_t)0.5 : v - (anv_phys_t)0.5);
}

#ifdef __cplusplus
}
#endif

#endif /* ANV_SCHEMA_H */
