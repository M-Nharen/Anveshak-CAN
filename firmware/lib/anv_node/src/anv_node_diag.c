#include "anv_node_diag.h"

#include <string.h>

#include "anv_node_internal.h"

#define LOG_KEY "anv_diag_log"
#define LOG_MAGIC 0x32474144u /* "DAG2" */

static uint32_t crc32_of(const uint8_t *p, size_t len)
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

static const anv_event_desc_t *find_event(const anv_node_t *n, uint16_t code, uint8_t *severity)
{
    const anv_type_desc_t *t = n->cfg.type;
    for (uint16_t i = 0; i < t->n_events; i++) {
        if (t->events[i].code == code) {
            *severity = t->events[i].severity;
            return &t->events[i];
        }
    }
    for (uint16_t i = 0; i < anv_protocol_type.n_events; i++) {
        if (anv_protocol_type.events[i].code == code) {
            *severity = anv_protocol_type.events[i].severity;
            return &anv_protocol_type.events[i];
        }
    }
    *severity = ANV_SEVERITY_INFO; /* unknown code: still worth reporting */
    return NULL;
}

/* True if an event of this code was let through within the last 1000/rate_limit ms. Updates the
 * table either way (recording this attempt's time once it's decided to let it through). */
static bool rate_limited(anv_node_t *n, uint16_t code)
{
    anv_node_diag_t *d = &n->diag;
    const uint32_t limit_hz = anv_node_i_param_u(n, ANV_PARAM_EVENT_RATE_LIMIT);
    const uint32_t period_ms = limit_hz == 0u ? 1000u : 1000u / limit_hz;
    int free_slot = -1;
    for (unsigned i = 0; i < ANV_NODE_DIAG_RATE_SLOTS; i++) {
        if (d->rate_code[i] == code && d->rate_ms[i] != 0u) {
            if ((uint32_t)(n->now_ms - d->rate_ms[i]) < period_ms) {
                return true;
            }
            d->rate_ms[i] = n->now_ms;
            return false;
        }
        if (d->rate_ms[i] == 0u && free_slot < 0) {
            free_slot = (int)i;
        }
    }
    const unsigned slot = free_slot >= 0 ? (unsigned)free_slot : (d->rate_next++ % ANV_NODE_DIAG_RATE_SLOTS);
    d->rate_code[slot] = code;
    d->rate_ms[slot] = n->now_ms;
    return false;
}

static void save_log(anv_node_t *n)
{
    if (n->cfg.storage == NULL) {
        return;
    }
    const anv_node_diag_t *d = &n->diag;
    uint8_t buf[4u + 4u + (size_t)ANV_NODE_DIAG_LOG_CAP * 8u + 4u];
    uint8_t *p = buf;
    memcpy(p, &(uint32_t){LOG_MAGIC}, 4u);
    p += 4;
    p[0] = d->count;
    p[1] = d->seq;
    p[2] = 0;
    p[3] = 0;
    p += 4;
    /* Written oldest-first (not the raw ring order), so a reload can treat the array as a
     * ring that simply hasn't wrapped yet. */
    for (unsigned i = 0; i < ANV_NODE_DIAG_LOG_CAP; i++) {
        const anv_diag_entry_t *e = i < d->count ? anv_node_diag_log_entry(n, (uint8_t)i) : &d->entries[i];
        p[0] = (uint8_t)e->code;
        p[1] = (uint8_t)(e->code >> 8);
        p[2] = (uint8_t)e->detail;
        p[3] = (uint8_t)(e->detail >> 8);
        p[4] = (uint8_t)e->uptime_s;
        p[5] = (uint8_t)(e->uptime_s >> 8);
        p[6] = (uint8_t)(e->severity | (e->cleared ? 0x04u : 0u));
        p[7] = e->seq;
        p += 8;
    }
    const uint32_t crc = crc32_of(buf, (size_t)(p - buf));
    memcpy(p, &crc, 4u);
    p += 4;
    (void)n->cfg.storage->save(n->cfg.storage_ctx, LOG_KEY, buf, (size_t)(p - buf));
}

void anv_node_i_diag_load(anv_node_t *n)
{
    anv_node_diag_t *d = &n->diag;
    memset(d, 0, sizeof *d);
    if (n->cfg.storage == NULL) {
        return;
    }
    uint8_t buf[4u + 4u + (size_t)ANV_NODE_DIAG_LOG_CAP * 8u + 4u];
    const size_t size = n->cfg.storage->load(n->cfg.storage_ctx, LOG_KEY, buf, sizeof buf);
    if (size != sizeof buf) {
        return; /* no saved log, or it doesn't match this build's layout: start empty */
    }
    uint32_t magic;
    memcpy(&magic, buf, 4u);
    if (magic != LOG_MAGIC) {
        return;
    }
    uint32_t crc;
    memcpy(&crc, buf + sizeof(buf) - 4u, 4u);
    if (crc != crc32_of(buf, sizeof(buf) - 4u)) {
        return; /* corrupt: start empty rather than show garbage */
    }
    d->count = buf[4];
    if (d->count > ANV_NODE_DIAG_LOG_CAP) {
        d->count = ANV_NODE_DIAG_LOG_CAP;
    }
    d->seq = buf[5];
    d->next = d->count;
    const uint8_t *p = buf + 8;
    for (unsigned i = 0; i < ANV_NODE_DIAG_LOG_CAP; i++) {
        anv_diag_entry_t *e = &d->entries[i];
        e->code = (uint16_t)(p[0] | (p[1] << 8));
        e->detail = (uint16_t)(p[2] | (p[3] << 8));
        e->uptime_s = (uint16_t)(p[4] | (p[5] << 8));
        e->severity = p[6] & 0x03u;
        e->cleared = (p[6] & 0x04u) != 0u;
        e->seq = p[7];
        p += 8;
    }
}

static void raise(anv_node_t *n, uint16_t code, uint16_t detail, bool cleared)
{
    anv_node_diag_t *d = &n->diag;
    uint8_t severity = ANV_SEVERITY_INFO;
    (void)find_event(n, code, &severity);
    d->seq++;
    anv_diag_entry_t *e = &d->entries[d->next % ANV_NODE_DIAG_LOG_CAP];
    e->code = code;
    e->detail = detail;
    e->uptime_s = (uint16_t)((n->now_ms - n->start_ms) / 1000u);
    e->severity = severity;
    e->cleared = cleared;
    e->seq = d->seq;
    d->next++;
    if (d->count < ANV_NODE_DIAG_LOG_CAP) {
        d->count++;
    }
    save_log(n);

    anv_event_t ev;
    ev.severity = severity;
    ev.reserved = 0;
    ev.cleared = cleared;
    ev.code = code;
    ev.seq = d->seq;
    ev.detail = detail;
    ev.uptime_s = e->uptime_s;
    uint8_t data[8];
    const uint8_t len = anv_event_pack(data, &ev);
    (void)anv_node_i_send(n, ANV_EVENT_PRIORITY, ANV_NODE_BROADCAST, ANV_EVENT_PORT, data, len);
}

void anv_node_event(anv_node_t *n, uint16_t code, uint16_t detail)
{
    if (rate_limited(n, code)) {
        return;
    }
    raise(n, code, detail, false);
}

void anv_node_event_clear(anv_node_t *n, uint16_t code, uint16_t detail)
{
    raise(n, code, detail, true);
}

uint8_t anv_node_diag_log_count(const anv_node_t *n)
{
    return n->diag.count;
}

const anv_diag_entry_t *anv_node_diag_log_entry(const anv_node_t *n, uint8_t i)
{
    if (i >= n->diag.count) {
        return NULL;
    }
    /* Oldest first: `next - count` is the index of the oldest entry still kept. */
    const uint8_t start = (uint8_t)(n->diag.next - n->diag.count);
    return &n->diag.entries[(uint8_t)(start + i) % ANV_NODE_DIAG_LOG_CAP];
}

/* ------------------------------------------------------------------------------------------ */
/* the `log` transfer service: send the black box to whoever asked                            */

uint16_t anv_node_i_diag_build_log(const anv_node_t *n, uint8_t *out, uint16_t cap)
{
    const uint8_t count = n->diag.count;
    uint16_t len = 0;
    if (cap < 1u) {
        return 0;
    }
    out[0] = 1u; /* log msg_type: DATA */
    len = 1;
    for (uint8_t i = 0; i < count && len + 8u <= cap; i++) {
        const anv_diag_entry_t *e = anv_node_diag_log_entry(n, i);
        uint8_t *p = out + len;
        p[0] = (uint8_t)e->code;
        p[1] = (uint8_t)(e->code >> 8);
        p[2] = (uint8_t)e->detail;
        p[3] = (uint8_t)(e->detail >> 8);
        p[4] = (uint8_t)e->uptime_s;
        p[5] = (uint8_t)(e->uptime_s >> 8);
        p[6] = (uint8_t)(e->severity | (e->cleared ? 0x04u : 0u));
        p[7] = e->seq;
        len = (uint16_t)(len + 8u);
    }
    return len;
}

#define LOG_MSG_REQUEST 0u

void anv_node_i_diag_handle(anv_node_t *n, anv_node_id_t src, const uint8_t *d, uint16_t len)
{
    (void)src; /* the shared transfer channel is already bound to this peer */
    if (len < 1u || d[0] != LOG_MSG_REQUEST) {
        return;
    }
    if (anv_isotp_tx_busy(&n->transfer)) {
        return; /* a previous reply is still going out; let it finish */
    }
    uint8_t *out = n->diag.tx_buf;
    out[0] = (uint8_t)ANV_TRANSFER_SERVICE_LOG;
    const uint16_t body_len = anv_node_i_diag_build_log(n, out + 1, (uint16_t)(sizeof n->diag.tx_buf - 1u));
    (void)anv_isotp_send(&n->transfer, out, (uint16_t)(1u + body_len), (uint32_t)n->now_ms * 1000u);
}
