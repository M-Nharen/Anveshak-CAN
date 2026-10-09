#include "anv_diag.h"

#include <string.h>

#include "errors.h"

#define BLOB_MAGIC 0x4C564E41u /* "ANVL" little-endian */
#define BLOB_VERSION 1u
#define ENTRY_BYTES 12u
#define BLOB_HEADER_BYTES 12u

bool anv_diag_severity_of(uint16_t code, uint8_t *severity)
{
    for (size_t i = 0; i < ROVER_ERROR_TABLE_LEN; i++) {
        if (ROVER_ERROR_TABLE[i].code == code) {
            if (severity != NULL) {
                *severity = (uint8_t)ROVER_ERROR_TABLE[i].severity;
            }
            return true;
        }
    }
    return false;
}

/* --- flash blob ---------------------------------------------------------------------------- */

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)(v & 0xFFFFu));
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static void put_entry(uint8_t *p, const anv_log_entry_t *e)
{
    put16(p, e->code);
    p[2] = e->severity;
    p[3] = e->flags;
    put16(p + 4, e->detail);
    put16(p + 6, e->uptime_ds);
    put16(p + 8, e->boot_count);
    p[10] = e->seq;
    p[11] = e->repeat;
}

static void get_entry(const uint8_t *p, anv_log_entry_t *e)
{
    e->code = get16(p);
    e->severity = p[2];
    e->flags = p[3];
    e->detail = get16(p + 4);
    e->uptime_ds = get16(p + 6);
    e->boot_count = get16(p + 8);
    e->seq = p[10];
    e->repeat = p[11];
}

size_t anv_diag_serialize(const anv_diag_t *d, uint8_t *out, size_t cap)
{
    if (cap < ANV_DIAG_BLOB_SIZE) {
        return 0;
    }
    memset(out, 0, ANV_DIAG_BLOB_SIZE);
    put32(out, BLOB_MAGIC);
    put32(out + 4, d->boot_count);
    put16(out + 8, d->flash_count);
    put16(out + 10, BLOB_VERSION);
    /* Oldest first, so loading is a plain append. */
    for (uint16_t i = 0; i < d->flash_count; i++) {
        uint16_t pos = (uint16_t)((d->flash_head + ANV_LOG_FLASH_ENTRIES - d->flash_count + i) % ANV_LOG_FLASH_ENTRIES);
        put_entry(out + BLOB_HEADER_BYTES + (size_t)i * ENTRY_BYTES, &d->flash[pos]);
    }
    put32(out + ANV_DIAG_BLOB_SIZE - 4u, crc32(out, ANV_DIAG_BLOB_SIZE - 4u));
    return ANV_DIAG_BLOB_SIZE;
}

bool anv_diag_deserialize(anv_diag_t *d, const uint8_t *in, size_t len)
{
    if (len < ANV_DIAG_BLOB_SIZE || get32(in) != BLOB_MAGIC || get16(in + 10) != BLOB_VERSION ||
        get32(in + ANV_DIAG_BLOB_SIZE - 4u) != crc32(in, ANV_DIAG_BLOB_SIZE - 4u)) {
        return false;
    }
    uint16_t count = get16(in + 8);
    if (count > ANV_LOG_FLASH_ENTRIES) {
        return false;
    }
    d->boot_count = (uint16_t)get32(in + 4);
    d->flash_count = 0;
    d->flash_head = 0;
    for (uint16_t i = 0; i < count; i++) {
        get_entry(in + BLOB_HEADER_BYTES + (size_t)i * ENTRY_BYTES, &d->flash[d->flash_head]);
        d->flash_head = (uint16_t)((d->flash_head + 1u) % ANV_LOG_FLASH_ENTRIES);
        d->flash_count++;
    }
    return true;
}

/* --- logs ---------------------------------------------------------------------------------- */

static void ram_append(anv_diag_t *d, const anv_log_entry_t *e)
{
    d->ram[d->ram_head] = *e;
    d->ram_head = (uint16_t)((d->ram_head + 1u) % ANV_LOG_RAM_ENTRIES);
    if (d->ram_count < ANV_LOG_RAM_ENTRIES) {
        d->ram_count++;
    }
}

static void flash_append(anv_diag_t *d, const anv_log_entry_t *e)
{
    d->flash[d->flash_head] = *e;
    d->flash_head = (uint16_t)((d->flash_head + 1u) % ANV_LOG_FLASH_ENTRIES);
    if (d->flash_count < ANV_LOG_FLASH_ENTRIES) {
        d->flash_count++;
    }
    d->flash_dirty = true;
}

uint16_t anv_diag_log_count(const anv_diag_t *d)
{
    return d->ram_count;
}

bool anv_diag_log_get(const anv_diag_t *d, uint16_t index, anv_log_entry_t *out)
{
    if (index >= d->ram_count) {
        return false;
    }
    uint16_t pos = (uint16_t)((d->ram_head + ANV_LOG_RAM_ENTRIES - 1u - index) % ANV_LOG_RAM_ENTRIES);
    *out = d->ram[pos];
    return true;
}

bool anv_diag_flush(anv_diag_t *d, uint32_t now_ms)
{
    if (d->board == NULL || d->board->log_save == NULL) {
        d->flash_dirty = false; /* no flash log on this board: RAM-only, nothing to retry */
        return false;
    }
    uint8_t blob[ANV_DIAG_BLOB_SIZE];
    (void)anv_diag_serialize(d, blob, sizeof(blob));
    d->last_flash_write_ms = now_ms;
    bool ok = d->board->log_save(d->board->ctx, blob, sizeof(blob)) == 0;
    if (ok) {
        d->flash_dirty = false;
        if (d->flash_error) {
            d->flash_error = false;
            anv_diag_clear(d, ROVER_ERR_FLASH_WRITE_FAIL, 0u, now_ms);
        }
    } else if (!d->flash_error) {
        d->flash_error = true;
        anv_diag_report(d, ROVER_ERR_FLASH_WRITE_FAIL, 0u, now_ms);
    }
    return ok;
}

void anv_diag_log_clear(anv_diag_t *d, uint32_t now_ms)
{
    d->ram_head = 0;
    d->ram_count = 0;
    d->flash_head = 0;
    d->flash_count = 0;
    d->flash_dirty = true;
    (void)anv_diag_flush(d, now_ms);
}

/* --- events -------------------------------------------------------------------------------- */

static uint16_t uptime_ds(const anv_diag_t *d, uint32_t now_ms)
{
    return (uint16_t)(((now_ms - d->boot_ms) / 100u) & 0xFFFFu);
}

/* Returns true when the event is dealt with (sent, or deliberately kept off the bus), false
 * when the bus queue was full and it should be retried. */
static bool emit(anv_diag_t *d, anv_diag_slot_t *s, bool cleared, uint16_t detail, uint32_t now_ms)
{
    anv_log_entry_t e = {
        .code = s->code,
        .severity = s->severity,
        .flags = (uint8_t)(cleared ? ANV_LOG_FLAG_CLEARED : 0u),
        .detail = detail,
        .uptime_ds = uptime_ds(d, now_ms),
        .boot_count = d->boot_count,
        .seq = 0,
        .repeat = s->coalesced,
    };

    bool allowed = d->sink != NULL && (d->gate == NULL || d->gate(d->sink_ctx, s->severity));
    if (allowed) {
        anv_diag_msg_t msg = {
            .severity = s->severity,
            .cleared = cleared,
            .code = s->code,
            .seq = d->seq,
            .detail = detail,
            .uptime_ds = e.uptime_ds,
        };
        if (!d->sink(d->sink_ctx, &msg)) {
            return false;
        }
        e.seq = d->seq;
        d->seq++;
        s->last_sent_ms = now_ms;
        s->sent_once = true;
    } else {
        e.flags |= ANV_LOG_FLAG_NOT_SENT;
    }
    s->coalesced = 0;

    ram_append(d, &e);
    if (s->severity >= (uint8_t)ROVER_SEVERITY_ERROR) {
        flash_append(d, &e);
        if (s->severity == (uint8_t)ROVER_SEVERITY_CRITICAL) {
            /* A critical may be followed by a reset; don't wait for the periodic write. */
            (void)anv_diag_flush(d, now_ms);
        }
    }
    return true;
}

static anv_diag_slot_t *find_slot(anv_diag_t *d, uint16_t code, uint8_t severity, anv_diag_slot_t *scratch)
{
    anv_diag_slot_t *free_slot = NULL;
    anv_diag_slot_t *victim = NULL;
    for (size_t i = 0; i < ANV_DIAG_SLOTS; i++) {
        anv_diag_slot_t *s = &d->slots[i];
        if (s->used && s->code == code) {
            return s;
        }
        if (!s->used && free_slot == NULL) {
            free_slot = s;
        }
        if (s->used && !s->active && !s->pending && (victim == NULL || s->last_sent_ms < victim->last_sent_ms)) {
            victim = s;
        }
    }
    anv_diag_slot_t *s = (free_slot != NULL) ? free_slot : victim;
    if (s == NULL) {
        /* Every slot holds an active condition: send this one without rate limiting. */
        s = scratch;
    }
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->code = code;
    s->severity = severity;
    return s;
}

static void submit(anv_diag_t *d, anv_diag_slot_t *s, bool cleared, uint16_t detail, uint32_t now_ms)
{
    bool limited = s->sent_once && (now_ms - s->last_sent_ms) < ANV_DIAG_MIN_INTERVAL_MS;
    if (limited || s->pending) {
        s->pending = true;
        s->pending_cleared = cleared;
        s->pending_detail = detail;
        if (s->coalesced < 255u) {
            s->coalesced++;
        }
        return;
    }
    if (!emit(d, s, cleared, detail, now_ms)) {
        s->pending = true;
        s->pending_cleared = cleared;
        s->pending_detail = detail;
    }
}

void anv_diag_report(anv_diag_t *d, uint16_t code, uint16_t detail, uint32_t now_ms)
{
    uint8_t sev;
    anv_diag_slot_t scratch;
    if (!anv_diag_severity_of(code, &sev)) {
        return;
    }
    anv_diag_slot_t *s = find_slot(d, code, sev, &scratch);
    if (s->active) {
        return; /* edge-triggered: still the same condition */
    }
    s->active = true;
    submit(d, s, false, detail, now_ms);
}

void anv_diag_clear(anv_diag_t *d, uint16_t code, uint16_t detail, uint32_t now_ms)
{
    for (size_t i = 0; i < ANV_DIAG_SLOTS; i++) {
        anv_diag_slot_t *s = &d->slots[i];
        if (s->used && s->code == code && s->active) {
            s->active = false;
            submit(d, s, true, detail, now_ms);
            return;
        }
    }
}

void anv_diag_event(anv_diag_t *d, uint16_t code, uint16_t detail, uint32_t now_ms)
{
    uint8_t sev;
    anv_diag_slot_t scratch;
    if (!anv_diag_severity_of(code, &sev)) {
        return;
    }
    submit(d, find_slot(d, code, sev, &scratch), false, detail, now_ms);
}

bool anv_diag_is_active(const anv_diag_t *d, uint16_t code)
{
    for (size_t i = 0; i < ANV_DIAG_SLOTS; i++) {
        if (d->slots[i].used && d->slots[i].code == code) {
            return d->slots[i].active;
        }
    }
    return false;
}

void anv_diag_tick(anv_diag_t *d, uint32_t now_ms)
{
    for (size_t i = 0; i < ANV_DIAG_SLOTS; i++) {
        anv_diag_slot_t *s = &d->slots[i];
        if (s->used && s->pending && (!s->sent_once || (now_ms - s->last_sent_ms) >= ANV_DIAG_MIN_INTERVAL_MS)) {
            if (emit(d, s, s->pending_cleared, s->pending_detail, now_ms)) {
                s->pending = false;
            }
        }
    }
    if (d->flash_dirty && !d->hold_flash && (now_ms - d->last_flash_write_ms) >= ANV_DIAG_FLASH_MIN_INTERVAL_MS) {
        (void)anv_diag_flush(d, now_ms);
    }
}

void anv_diag_set_sink(anv_diag_t *d, anv_diag_sink_t sink, anv_diag_gate_t gate, void *ctx)
{
    d->sink = sink;
    d->gate = gate;
    d->sink_ctx = ctx;
}

void anv_diag_init(anv_diag_t *d, const anv_board_hooks_t *board, uint32_t now_ms)
{
    memset(d, 0, sizeof(*d));
    d->board = board;
    d->boot_ms = now_ms;
    d->last_flash_write_ms = now_ms;

    if (board != NULL && board->log_load != NULL) {
        uint8_t blob[ANV_DIAG_BLOB_SIZE];
        if (board->log_load(board->ctx, blob, sizeof(blob)) == 0) {
            if (!anv_diag_deserialize(d, blob, sizeof(blob))) {
                /* Blank or corrupt page: start a fresh log. */
                d->boot_count = 0;
                d->flash_count = 0;
                d->flash_head = 0;
            }
        }
    }

    /* Earlier boots' errors become the oldest part of the ring. */
    for (uint16_t i = 0; i < d->flash_count; i++) {
        uint16_t pos = (uint16_t)((d->flash_head + ANV_LOG_FLASH_ENTRIES - d->flash_count + i) % ANV_LOG_FLASH_ENTRIES);
        ram_append(d, &d->flash[pos]);
    }

    d->boot_count++;
    d->flash_dirty = true;
    (void)anv_diag_flush(d, now_ms);
}
