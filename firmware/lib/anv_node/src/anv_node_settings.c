/* anv_node settings, stream rates, saving, and the SERVICE port. */
#include <string.h>

#include "anv_node_internal.h"

/* Saved settings: a header, the label, then (kind, index, value) entries. Entries are matched by
 * index and checked against the current type on load, so a changed schema keeps what still
 * fits and falls back to defaults for the rest. */
#define SAVE_KEY_PARAMS "anv_params"
#define SAVE_MAGIC 0x314E5641u /* "AVN1" */
#define SAVE_HEADER 12u
#define SAVE_ENTRY 8u
#define SAVE_KIND_PARAM 0u
#define SAVE_KIND_RATE 1u
#define SAVE_MAX (SAVE_HEADER + ANV_NODE_LABEL_LEN + SAVE_ENTRY * (ANV_NODE_MAX_PARAMS + ANV_NODE_MAX_STREAMS))

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32(const uint8_t *p, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (unsigned b = 0; b < 8u; b++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

/* ------------------------------------------------------------------------------------------ */
/* settings                                                                                    */

static int find_param(const anv_node_t *n, uint16_t index)
{
    for (uint16_t i = 0; i < n->n_params; i++) {
        if (n->params[i]->index == index) {
            return (int)i;
        }
    }
    return -1;
}

static bool type_ok(uint8_t type, anv_value_t v)
{
    switch (type) {
    case ANV_VT_BOOL: return v.u <= 1u;
    case ANV_VT_U8: return v.u <= 0xFFu;
    case ANV_VT_U16: return v.u <= 0xFFFFu;
    case ANV_VT_U32: return true;
    case ANV_VT_I8: return v.i >= -128 && v.i <= 127;
    case ANV_VT_I16: return v.i >= -32768 && v.i <= 32767;
    case ANV_VT_I32: return true;
    case ANV_VT_F32: return v.f == v.f && v.f - v.f == 0.0f; /* not NaN, not infinite */
    default: return false;
    }
}

/* -1 below, 0 equal, 1 above, by the parameter's type */
static int compare(uint8_t type, anv_value_t a, anv_value_t b)
{
    switch (type) {
    case ANV_VT_I8:
    case ANV_VT_I16:
    case ANV_VT_I32: return a.i < b.i ? -1 : a.i > b.i;
    case ANV_VT_F32: return a.f < b.f ? -1 : a.f > b.f;
    default: return a.u < b.u ? -1 : a.u > b.u;
    }
}

static bool value_ok(const anv_param_desc_t *p, anv_value_t v)
{
    if (!type_ok(p->type, v)) {
        return false;
    }
    if ((p->flags & ANV_PARAM_HAS_MIN) != 0u && compare(p->type, v, p->min) < 0) {
        return false;
    }
    if ((p->flags & ANV_PARAM_HAS_MAX) != 0u && compare(p->type, v, p->max) > 0) {
        return false;
    }
    return true;
}

uint32_t anv_node_i_param_u(const anv_node_t *n, uint16_t index)
{
    anv_value_t v = {0};
    (void)anv_node_get(n, index, &v);
    return v.u;
}

static uint32_t rate_period_ms(uint32_t rate_mhz)
{
    if (rate_mhz == 0u) {
        return 0u;
    }
    const uint32_t ms = (1000000u + rate_mhz / 2u) / rate_mhz;
    return ms == 0u ? 1u : ms;
}

/* Settings the runtime itself uses take effect at once. */
static void builtin_setting_changed(anv_node_t *n, uint16_t index)
{
    if (index == ANV_PARAM_HEARTBEAT_HZ) {
        n->heartbeat_next_ms = n->now_ms;
    } else if (index == ANV_PARAM_DEBUG_LEVEL) {
        n->debug_timed = false;
    }
}

void anv_node_i_load_defaults(anv_node_t *n)
{
    for (uint16_t i = 0; i < n->n_params; i++) {
        n->values[i] = n->params[i]->def;
    }
    for (uint8_t i = 0; i < n->n_streams; i++) {
        n->streams[i].rate_mhz = n->streams[i].msg->rate_default_mhz;
        n->streams[i].period_ms = rate_period_ms(n->streams[i].rate_mhz);
    }
    memset(n->label, 0, sizeof n->label);
}

static int find_stream(const anv_node_t *n, uint16_t port)
{
    for (uint8_t i = 0; i < n->n_streams; i++) {
        if (n->streams[i].msg->port == port) {
            return (int)i;
        }
    }
    return -1;
}

static bool rate_ok(const anv_msg_desc_t *m, uint32_t mhz)
{
    return mhz >= m->rate_min_mhz && mhz <= m->rate_max_mhz;
}

void anv_node_i_load_saved(anv_node_t *n)
{
    const anv_node_storage_t *st = n->cfg.storage;
    if (st == NULL) {
        return;
    }
    uint8_t buf[SAVE_MAX];
    const size_t size = st->load(n->cfg.storage_ctx, SAVE_KEY_PARAMS, buf, sizeof buf);
    if (size < SAVE_HEADER + ANV_NODE_LABEL_LEN) {
        return;
    }
    const uint16_t count = get_u16(buf + 4);
    if (get_u32(buf) != SAVE_MAGIC || size != SAVE_HEADER + ANV_NODE_LABEL_LEN + (size_t)count * SAVE_ENTRY ||
        get_u32(buf + 8) != crc32(buf + SAVE_HEADER, size - SAVE_HEADER)) {
        return; /* corrupt: keep the defaults (the params_corrupt event comes with L5) */
    }
    memcpy(n->label, buf + SAVE_HEADER, ANV_NODE_LABEL_LEN);
    const uint8_t *e = buf + SAVE_HEADER + ANV_NODE_LABEL_LEN;
    for (uint16_t k = 0; k < count; k++, e += SAVE_ENTRY) {
        const uint16_t index = get_u16(e + 2);
        anv_value_t v;
        v.u = get_u32(e + 4);
        if (e[0] == SAVE_KIND_PARAM) {
            const int i = find_param(n, index);
            if (i >= 0 && (n->params[i]->flags & ANV_PARAM_PERSIST) != 0u && value_ok(n->params[i], v)) {
                n->values[i] = v;
            }
        } else if (e[0] == SAVE_KIND_RATE) {
            const int i = find_stream(n, index);
            if (i >= 0 && rate_ok(n->streams[i].msg, v.u)) {
                n->streams[i].rate_mhz = v.u;
                n->streams[i].period_ms = rate_period_ms(v.u);
            }
        }
    }
}

uint8_t anv_node_save(anv_node_t *n)
{
    const anv_node_storage_t *st = n->cfg.storage;
    if (st == NULL) {
        return ANV_SERVICE_STATUS_STORAGE_ERROR;
    }
    uint8_t buf[SAVE_MAX];
    uint16_t count = 0;
    uint8_t *e = buf + SAVE_HEADER + ANV_NODE_LABEL_LEN;
    for (uint16_t i = 0; i < n->n_params; i++) {
        if ((n->params[i]->flags & ANV_PARAM_PERSIST) == 0u) {
            continue;
        }
        e[0] = SAVE_KIND_PARAM;
        e[1] = 0;
        put_u16(e + 2, n->params[i]->index);
        put_u32(e + 4, n->values[i].u);
        e += SAVE_ENTRY;
        count++;
    }
    for (uint8_t i = 0; i < n->n_streams; i++) {
        e[0] = SAVE_KIND_RATE;
        e[1] = 0;
        put_u16(e + 2, n->streams[i].msg->port);
        put_u32(e + 4, n->streams[i].rate_mhz);
        e += SAVE_ENTRY;
        count++;
    }
    memcpy(buf + SAVE_HEADER, n->label, ANV_NODE_LABEL_LEN);
    const size_t size = (size_t)(e - buf);
    put_u32(buf, SAVE_MAGIC);
    put_u16(buf + 4, count);
    put_u16(buf + 6, 0);
    put_u32(buf + 8, crc32(buf + SAVE_HEADER, size - SAVE_HEADER));
    if (!st->save(n->cfg.storage_ctx, SAVE_KEY_PARAMS, buf, size)) {
        return ANV_SERVICE_STATUS_STORAGE_ERROR;
    }
    n->unsaved = false;
    return ANV_SERVICE_STATUS_OK;
}

const anv_param_desc_t *anv_node_param(const anv_node_t *n, uint16_t index)
{
    const int i = find_param(n, index);
    return i < 0 ? NULL : n->params[i];
}

bool anv_node_get(const anv_node_t *n, uint16_t index, anv_value_t *out)
{
    const int i = find_param(n, index);
    if (i < 0) {
        return false;
    }
    *out = n->values[i];
    return true;
}

uint8_t anv_node_i_set_param(anv_node_t *n, uint16_t index, anv_value_t v, bool from_host)
{
    const int i = find_param(n, index);
    if (i < 0) {
        return ANV_SERVICE_STATUS_BAD_INDEX;
    }
    const anv_param_desc_t *p = n->params[i];
    if (from_host && (p->flags & ANV_PARAM_READ_ONLY) != 0u) {
        return ANV_SERVICE_STATUS_READ_ONLY;
    }
    if ((p->flags & ANV_PARAM_DISARMED_ONLY) != 0u && anv_node_i_armed(n)) {
        return ANV_SERVICE_STATUS_NOT_WHILE_ARMED;
    }
    if (!value_ok(p, v)) {
        return ANV_SERVICE_STATUS_OUT_OF_RANGE;
    }
    const bool changed = n->values[i].u != v.u;
    n->values[i] = v;
    if (changed) {
        if ((p->flags & ANV_PARAM_PERSIST) != 0u) {
            n->unsaved = true;
        }
        builtin_setting_changed(n, index);
        if (n->cfg.on_setting_changed != NULL) {
            n->cfg.on_setting_changed(n, p, v, n->cfg.user);
        }
    }
    return ANV_SERVICE_STATUS_OK;
}

uint8_t anv_node_set(anv_node_t *n, uint16_t index, anv_value_t value)
{
    return anv_node_i_set_param(n, index, value, false);
}

uint8_t anv_node_restore_defaults(anv_node_t *n)
{
    if (anv_node_i_armed(n)) {
        return ANV_SERVICE_STATUS_NOT_WHILE_ARMED;
    }
    anv_value_t old[ANV_NODE_MAX_PARAMS];
    memcpy(old, n->values, sizeof(anv_value_t) * n->n_params);
    anv_node_i_load_defaults(n);
    for (uint16_t i = 0; i < n->n_params; i++) {
        if (old[i].u != n->values[i].u) {
            builtin_setting_changed(n, n->params[i]->index);
            if (n->cfg.on_setting_changed != NULL) {
                n->cfg.on_setting_changed(n, n->params[i], n->values[i], n->cfg.user);
            }
        }
    }
    for (uint8_t i = 0; i < n->n_streams; i++) {
        n->streams[i].next_ms = n->now_ms + n->streams[i].period_ms;
    }
    n->unsaved = true;
    return ANV_SERVICE_STATUS_OK;
}

const char *anv_node_label(const anv_node_t *n)
{
    return n->label;
}

/* ------------------------------------------------------------------------------------------ */
/* streams                                                                                     */

uint32_t anv_node_stream_rate(const anv_node_t *n, uint16_t port)
{
    const int i = find_stream(n, port);
    return i < 0 ? 0u : n->streams[i].rate_mhz;
}

uint8_t anv_node_set_stream_rate(anv_node_t *n, uint16_t port, uint32_t rate_mhz)
{
    const int i = find_stream(n, port);
    if (i < 0) {
        return ANV_SERVICE_STATUS_BAD_INDEX;
    }
    anv_node_stream_t *s = &n->streams[i];
    if (!rate_ok(s->msg, rate_mhz)) {
        return ANV_SERVICE_STATUS_OUT_OF_RANGE;
    }
    if (s->rate_mhz != rate_mhz) {
        s->rate_mhz = rate_mhz;
        s->period_ms = rate_period_ms(rate_mhz);
        s->next_ms = n->now_ms + s->period_ms;
        n->unsaved = true;
    }
    return ANV_SERVICE_STATUS_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* service                                                                                     */

static uint32_t info_page(const anv_node_t *n, uint16_t page, bool *ok)
{
    const anv_link_stats_t *ls = anv_link_stats(n->cfg.link);
    const uint8_t *mac = n->cfg.mac;
    *ok = true;
    switch (page) {
    case ANV_INFO_PAGE_FW_BUILD: return n->cfg.info.fw_build;
    case ANV_INFO_PAGE_FW_VERSION: return n->cfg.info.fw_version;
    case ANV_INFO_PAGE_SCHEMA_HASH: return n->cfg.type->wire_hash;
    case ANV_INFO_PAGE_TYPE_ID: return n->cfg.type->type_id;
    case ANV_INFO_PAGE_CHIP: return n->cfg.info.chip;
    case ANV_INFO_PAGE_MAC_LOW:
        return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    case ANV_INFO_PAGE_MAC_HIGH: return ((uint32_t)mac[0] << 8) | mac[1];
    case ANV_INFO_PAGE_BOOT_COUNT: return n->cfg.info.boot_count;
    case ANV_INFO_PAGE_RESET_REASON: return n->cfg.info.reset_reason;
    case ANV_INFO_PAGE_UPTIME_MS: return n->now_ms - n->start_ms;
    case ANV_INFO_PAGE_CAN_TEC: return ls->tec;
    case ANV_INFO_PAGE_CAN_REC: return ls->rec;
    case ANV_INFO_PAGE_BUS_OFF_COUNT: return ls->bus_off_count;
    case ANV_INFO_PAGE_RX_FRAMES: return ls->rx_frames;
    case ANV_INFO_PAGE_TX_FRAMES: return ls->tx_frames;
    case ANV_INFO_PAGE_RX_DROPPED: return ls->rx_overflow;
    default: *ok = false; return 0u;
    }
}

static uint8_t run_action(anv_node_t *n, uint16_t index, anv_value_t arg)
{
    const anv_type_desc_t *t = n->cfg.type;
    for (uint16_t i = 0; i < t->n_actions; i++) {
        const anv_action_desc_t *a = &t->actions[i];
        if (a->index != index) {
            continue;
        }
        if ((a->flags & ANV_ACTION_DISARMED_ONLY) != 0u && anv_node_i_armed(n)) {
            return ANV_SERVICE_STATUS_NOT_WHILE_ARMED;
        }
        if (a->arg_type != ANV_VT_NONE && !type_ok(a->arg_type, arg)) {
            return ANV_SERVICE_STATUS_OUT_OF_RANGE;
        }
        if (n->cfg.on_action == NULL) {
            return ANV_SERVICE_STATUS_FAILED;
        }
        return n->cfg.on_action(n, a, arg, n->cfg.user);
    }
    return ANV_SERVICE_STATUS_BAD_INDEX;
}

void anv_node_i_on_service(anv_node_t *n, anv_node_id_t src, const anv_link_frame_t *f)
{
    anv_service_t r;
    if (!anv_service_unpack(&r, f->data, f->len) || r.status != ANV_SERVICE_STATUS_OK) {
        return; /* responses (non-zero status) aren't for us */
    }
    n->stats.services++;
    anv_value_t v;
    v.u = r.value;
    uint8_t status = ANV_SERVICE_STATUS_OK;
    bool ok = true;
    switch (r.op) {
    case ANV_SERVICE_OP_GET:
        if (!anv_node_get(n, r.index, &v)) {
            status = ANV_SERVICE_STATUS_BAD_INDEX;
        }
        break;
    case ANV_SERVICE_OP_SET:
        status = anv_node_i_set_param(n, r.index, v, true);
        (void)anv_node_get(n, r.index, &v);
        break;
    case ANV_SERVICE_OP_SAVE: status = anv_node_save(n); break;
    case ANV_SERVICE_OP_RESTORE: status = anv_node_restore_defaults(n); break;
    case ANV_SERVICE_OP_INFO:
        v.u = info_page(n, r.index, &ok);
        status = ok ? ANV_SERVICE_STATUS_OK : ANV_SERVICE_STATUS_BAD_INDEX;
        break;
    case ANV_SERVICE_OP_ACTION: status = run_action(n, r.index, v); break;
    case ANV_SERVICE_OP_GET_RATE:
        if (find_stream(n, r.index) < 0) {
            status = ANV_SERVICE_STATUS_BAD_INDEX;
        }
        v.u = anv_node_stream_rate(n, r.index);
        break;
    case ANV_SERVICE_OP_SET_RATE:
        status = anv_node_set_stream_rate(n, r.index, v.u);
        v.u = anv_node_stream_rate(n, r.index);
        break;
    case ANV_SERVICE_OP_LABEL_GET:
    case ANV_SERVICE_OP_LABEL_SET:
        if (r.index >= ANV_NODE_LABEL_LEN / 4u) {
            status = ANV_SERVICE_STATUS_BAD_INDEX;
        } else if (r.op == ANV_SERVICE_OP_LABEL_SET) {
            uint8_t chunk[4];
            put_u32(chunk, v.u);
            if (memcmp(n->label + 4u * r.index, chunk, 4u) != 0) {
                memcpy(n->label + 4u * r.index, chunk, 4u);
                n->unsaved = true;
            }
        } else {
            v.u = get_u32((const uint8_t *)n->label + 4u * r.index);
        }
        break;
    default: status = ANV_SERVICE_STATUS_BAD_OP; break;
    }
    anv_service_t resp = r;
    resp.status = status;
    resp.value = v.u;
    uint8_t data[8];
    const uint8_t len = anv_service_pack(data, &resp);
    (void)anv_node_i_send(n, ANV_SERVICE_PRIORITY, src, ANV_SERVICE_PORT, data, len);
}

