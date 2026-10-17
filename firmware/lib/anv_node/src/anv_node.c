#include "anv_node_internal.h"

#include <string.h>


static bool due(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

static uint32_t next_random(anv_node_t *n)
{
    /* xorshift32: only spreads out ID requests, so quality hardly matters */
    uint32_t x = n->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    n->rng = x;
    return x;
}

/* ------------------------------------------------------------------------------------------ */
/* sending                                                                                     */

bool anv_node_i_send(anv_node_t *n, uint8_t priority, anv_node_id_t dst, uint16_t port, const uint8_t *data,
                     uint8_t len)
{
    const anv_proto_addr_t a = {priority, dst, port, n->id};
    anv_link_frame_t f;
    if (!anv_proto_frame(&f, &a, data, len) || anv_link_send(n->cfg.link, &f) != ANV_LINK_OK) {
        n->stats.tx_dropped++;
        return false;
    }
    return true;
}

static void send_heartbeat(anv_node_t *n)
{
    const anv_link_stats_t *ls = anv_link_stats(n->cfg.link);
    anv_value_t dbg = {0};
    (void)anv_node_get(n, ANV_PARAM_DEBUG_LEVEL, &dbg);
    anv_heartbeat_t hb;
    memset(&hb, 0, sizeof hb);
    hb.state = n->state;
    hb.can_state = (uint8_t)ls->state; /* anv_link_state_t matches can_error_state 0..2 */
    hb.debug_level = (uint8_t)dbg.u;
    hb.unsaved_params = n->unsaved;
    hb.leader_seen = n->have_leader;
    hb.fault_active = n->fault_active || n->fault_latched;
    hb.type_id = n->cfg.type->type_id;
    hb.schema_hash = (uint16_t)n->cfg.type->wire_hash;
    hb.uptime_s = (uint16_t)((n->now_ms - n->start_ms) / 1000u);
    uint8_t data[8];
    const uint8_t len = anv_heartbeat_pack(data, &hb);
    (void)anv_node_i_send(n, ANV_HEARTBEAT_PRIORITY, ANV_NODE_BROADCAST, ANV_HEARTBEAT_PORT, data, len);
}

static void send_id_request(anv_node_t *n)
{
    anv_id_request_t r;
    memcpy(r.mac, n->cfg.mac, sizeof r.mac);
    r.type_id = n->cfg.type->type_id;
    uint8_t data[8];
    const uint8_t len = anv_id_request_pack(data, &r);
    (void)anv_node_i_send(n, ANV_ID_REQUEST_PRIORITY, ANV_NODE_BROADCAST, ANV_ID_REQUEST_PORT, data, len);
}

/* ------------------------------------------------------------------------------------------ */
/* state                                                                                       */

bool anv_node_i_armed(const anv_node_t *n)
{
    return n->state == ANV_NODE_STATE_ARMED;
}

/* The state follows from the latches, in order of precedence. */
static void set_state(anv_node_t *n, bool armed)
{
    uint8_t s;
    if (!n->begun) {
        s = ANV_NODE_STATE_BOOT;
    } else if (n->id == ANV_NODE_UNASSIGNED) {
        s = ANV_NODE_STATE_UNASSIGNED;
    } else if (n->estop_latched) {
        s = ANV_NODE_STATE_ESTOP;
    } else if (n->fault_latched) {
        s = ANV_NODE_STATE_FAULT;
    } else if (armed) {
        s = ANV_NODE_STATE_ARMED;
    } else {
        s = ANV_NODE_STATE_IDLE;
    }
    const uint8_t old = n->state;
    if (s == old) {
        return;
    }
    n->state = s;
    if (old == ANV_NODE_STATE_ARMED) {
        for (uint8_t i = 0; i < n->n_commands; i++) {
            n->commands[i].held = false;
        }
        n->cfg.outputs_off(n, n->cfg.user);
    }
    if (n->cfg.on_state_changed != NULL) {
        n->cfg.on_state_changed(n, old, s, n->cfg.user);
    }
}

static void disarm(anv_node_t *n)
{
    set_state(n, false);
}

static void apply_filters(anv_node_t *n)
{
    anv_link_filter_t f[2];
    const uint8_t count = anv_proto_node_filters(n->id, f);
    (void)anv_link_set_filters(n->cfg.link, f, count);
}

static void use_id(anv_node_t *n, anv_node_id_t id)
{
    n->id = id;
    if (!n->begun) {
        return;
    }
    apply_filters(n);
    if (id == ANV_NODE_UNASSIGNED) {
        n->id_request_next_ms = n->now_ms + ANV_NODE_ID_REQUEST_FIRST_MS + next_random(n) % ANV_NODE_ID_REQUEST_JITTER_MS;
    } else {
        n->heartbeat_next_ms = n->now_ms; /* announce ourselves at once */
        for (uint8_t i = 0; i < n->n_streams; i++) {
            n->streams[i].next_ms = n->now_ms + n->streams[i].period_ms;
        }
    }
    set_state(n, false);
}

static bool valid_id(uint32_t id)
{
    return id >= ANV_NODE_FIRST && id <= ANV_NODE_LAST;
}

/* ------------------------------------------------------------------------------------------ */
/* sending the type's messages                                                                */

static const anv_msg_desc_t *find_type_msg(const anv_node_t *n, uint16_t port)
{
    const anv_type_desc_t *t = n->cfg.type;
    for (uint16_t i = 0; i < t->n_messages; i++) {
        if (t->messages[i].port == port) {
            return &t->messages[i];
        }
    }
    return NULL;
}

bool anv_node_send(anv_node_t *n, uint16_t port, const uint8_t *data, uint8_t len)
{
    const anv_msg_desc_t *m = find_type_msg(n, port);
    if (m == NULL || m->kind == ANV_KIND_COMMAND || n->id == ANV_NODE_UNASSIGNED || len > 8u) {
        return false;
    }
    return anv_node_i_send(n, m->priority, ANV_NODE_BROADCAST, port, data, len);
}

bool anv_node_send_frame(anv_node_t *n, anv_link_frame_t *frame)
{
    if (n->id == ANV_NODE_UNASSIGNED) {
        return false;
    }
    frame->id = (frame->id & ~(uint32_t)ANV_PROTO_SRC_MASK) | ((uint32_t)n->id << ANV_ID_SRC_SHIFT);
    if (anv_link_send(n->cfg.link, frame) != ANV_LINK_OK) {
        n->stats.tx_dropped++;
        return false;
    }
    return true;
}

static void run_streams(anv_node_t *n)
{
    for (uint8_t i = 0; i < n->n_streams; i++) {
        anv_node_stream_t *s = &n->streams[i];
        if (s->period_ms == 0u || !due(n->now_ms, s->next_ms)) {
            continue;
        }
        s->next_ms += s->period_ms;
        if (due(n->now_ms, s->next_ms)) {
            s->next_ms = n->now_ms + s->period_ms; /* fell behind: don't send a burst */
        }
        uint8_t data[8] = {0};
        if (n->cfg.fill_stream != NULL && n->cfg.fill_stream(n, s->msg, data, n->cfg.user)) {
            (void)anv_node_i_send(n, s->msg->priority, ANV_NODE_BROADCAST, s->msg->port, data, s->msg->length);
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* leader                                                                                      */

static void lose_leader(anv_node_t *n)
{
    n->have_leader = false;
    disarm(n);
}

static void on_host_heartbeat(anv_node_t *n, anv_node_id_t src, const anv_link_frame_t *f)
{
    anv_host_heartbeat_t hb;
    if (!anv_host_heartbeat_unpack(&hb, f->data, f->len)) {
        return;
    }
    const bool is_current = n->have_leader && src == n->leader;
    if (hb.role != ANV_HOST_ROLE_LEADER) {
        if (is_current) { /* our leader stepped down */
            n->stats.leader_changes++;
            lose_leader(n);
        }
        return;
    }
    if (is_current && hb.term == n->leader_term) {
        n->leader_seen_ms = n->now_ms;
        return;
    }
    /* Highest term wins. A new term, even from the same host, means a new leadership: disarm. */
    if (n->have_leader && (uint16_t)(hb.term - n->leader_term) >= 0x8000u) {
        return; /* older term */
    }
    if (n->have_leader && hb.term == n->leader_term) {
        return; /* same term from another host: the hosts sort that out between them */
    }
    if (n->have_leader) {
        n->stats.leader_changes++;
        disarm(n);
    }
    n->have_leader = true;
    n->leader = src;
    n->leader_term = hb.term;
    n->leader_seen_ms = n->now_ms;
}

static bool from_leader(const anv_node_t *n, anv_node_id_t src)
{
    return n->have_leader && src == n->leader;
}

/* ------------------------------------------------------------------------------------------ */
/* protocol messages                                                                           */

static void latch_estop(anv_node_t *n)
{
    if (!n->estop_latched) {
        n->stats.estops++;
    }
    n->estop_latched = true;
    disarm(n);
}

void anv_node_estop(anv_node_t *n, uint8_t reason)
{
    latch_estop(n);
    const anv_estop_t e = {0xE5u, reason};
    uint8_t data[8];
    const uint8_t len = anv_estop_pack(data, &e);
    /* From 255 too if we have no ID yet: E-stop must always get out. */
    (void)anv_node_i_send(n, ANV_ESTOP_PRIORITY, ANV_NODE_BROADCAST, ANV_ESTOP_PORT, data, len);
}

void anv_node_set_fault(anv_node_t *n, bool active)
{
    n->fault_active = active;
    if (active) {
        n->fault_latched = true;
        disarm(n);
    }
}

void anv_node_disarm(anv_node_t *n)
{
    disarm(n);
}

bool anv_node_save_id(anv_node_t *n, anv_node_id_t id)
{
    if (!valid_id(id) || anv_node_i_armed(n) || n->cfg.storage == NULL ||
        !n->cfg.storage->save(n->cfg.storage_ctx, ANV_NODE_KEY_ID, &id, 1u)) {
        return false;
    }
    n->id_saved = true;
    if (id != n->id) {
        use_id(n, id);
    }
    return true;
}

bool anv_node_forget_id(anv_node_t *n)
{
    if (anv_node_i_armed(n) || n->cfg.fixed_id != 0u) {
        return false;
    }
    if (n->cfg.storage != NULL) {
        (void)n->cfg.storage->erase(n->cfg.storage_ctx, ANV_NODE_KEY_ID);
    }
    n->id_saved = false;
    use_id(n, ANV_NODE_UNASSIGNED);
    return true;
}

static void on_id_assign(anv_node_t *n, const anv_link_frame_t *f)
{
    anv_id_assign_t a;
    if (n->id != ANV_NODE_UNASSIGNED || !anv_id_assign_unpack(&a, f->data, f->len) ||
        memcmp(a.mac, n->cfg.mac, sizeof a.mac) != 0 || !valid_id(a.node_id)) {
        return;
    }
    if (n->cfg.save_assigned_id && n->cfg.storage != NULL &&
        n->cfg.storage->save(n->cfg.storage_ctx, ANV_NODE_KEY_ID, &a.node_id, 1u)) {
        n->id_saved = true;
    }
    use_id(n, a.node_id);
}

static void on_nmt(anv_node_t *n, const anv_link_frame_t *f)
{
    anv_nmt_t m;
    if (!anv_nmt_unpack(&m, f->data, f->len) || m.term != n->leader_term) {
        return;
    }
    switch (m.command) {
    case ANV_NMT_COMMAND_ARM:
        if (n->state == ANV_NODE_STATE_IDLE) {
            for (uint8_t i = 0; i < n->n_commands; i++) {
                n->commands[i].last_ms = n->now_ms;
                n->commands[i].held = false;
            }
            set_state(n, true);
        }
        break;
    case ANV_NMT_COMMAND_DISARM:
        disarm(n);
        break;
    case ANV_NMT_COMMAND_RESET:
        disarm(n);
        if (n->cfg.on_reset != NULL) {
            n->cfg.on_reset(n, n->cfg.user);
        }
        break;
    case ANV_NMT_COMMAND_FAULT_RESET:
        if (n->fault_latched && !n->fault_active) {
            n->fault_latched = false;
            set_state(n, false);
        }
        break;
    case ANV_NMT_COMMAND_IDENTIFY:
        if (n->cfg.on_identify != NULL) {
            n->cfg.on_identify(n, n->cfg.user);
        }
        break;
    case ANV_NMT_COMMAND_FORGET_ID:
        if (n->state == ANV_NODE_STATE_IDLE) {
            (void)anv_node_forget_id(n);
        }
        break;
    default:
        break;
    }
}

static void deliver(anv_node_t *n, anv_node_command_t *c, const uint8_t *data, uint8_t len)
{
    n->stats.commands++;
    if (n->cfg.on_command != NULL) {
        n->cfg.on_command(n, c->msg, data, len, n->cfg.user);
    }
}

static void on_sync(anv_node_t *n, const anv_link_frame_t *f)
{
    anv_sync_t s;
    if (!anv_sync_unpack(&s, f->data, f->len)) {
        return;
    }
    for (uint8_t i = 0; i < n->n_commands; i++) {
        anv_node_command_t *c = &n->commands[i];
        if (c->held && anv_node_i_armed(n)) {
            c->held = false;
            deliver(n, c, c->held_data, c->held_len);
        }
    }
    if (n->cfg.on_sync != NULL) {
        n->cfg.on_sync(n, s.counter, n->cfg.user);
    }
}

static void on_debug_level(anv_node_t *n, const anv_link_frame_t *f)
{
    anv_debug_level_t d;
    if (!anv_debug_level_unpack(&d, f->data, f->len)) {
        return;
    }
    const anv_param_desc_t *p = anv_node_param(n, ANV_PARAM_DEBUG_LEVEL);
    anv_value_t v;
    v.u = d.level;
    if (p != NULL && (p->flags & ANV_PARAM_HAS_MAX) != 0u && v.u > p->max.u) {
        v.u = p->max.u;
    }
    (void)anv_node_i_set_param(n, ANV_PARAM_DEBUG_LEVEL, v, false);
    n->debug_timed = d.timeout_s != 0u;
    n->debug_until_ms = n->now_ms + (uint32_t)d.timeout_s * 1000u;
}

/* ------------------------------------------------------------------------------------------ */
/* frame dispatch                                                                             */

/* True if the runtime handled the frame. */
static bool on_protocol_frame(anv_node_t *n, const anv_link_frame_t *f)
{
    const uint16_t port = anv_id_port(f->id);
    const anv_node_id_t src = anv_id_src(f->id);
    const anv_node_id_t dst = anv_id_dst(f->id);
    const bool assigned = n->id != ANV_NODE_UNASSIGNED;

    switch (port) {
    case ANV_ESTOP_PORT: {
        anv_estop_t e;
        if (anv_estop_unpack(&e, f->data, f->len)) {
            latch_estop(n);
        }
        return true;
    }
    case ANV_HOST_HEARTBEAT_PORT: on_host_heartbeat(n, src, f); return true;
    case ANV_ID_ASSIGN_PORT: on_id_assign(n, f); return true;
    case ANV_ESTOP_RELEASE_PORT: {
        anv_estop_release_t r;
        if (from_leader(n, src) && anv_estop_release_unpack(&r, f->data, f->len) && r.term == n->leader_term) {
            n->estop_latched = false;
            set_state(n, false);
        }
        return true;
    }
    case ANV_NMT_PORT:
        if (assigned && from_leader(n, src)) {
            on_nmt(n, f);
        }
        return true;
    case ANV_SYNC_PORT:
        if (from_leader(n, src)) {
            on_sync(n, f);
        }
        return true;
    case ANV_DEBUG_LEVEL_PORT:
        if (from_leader(n, src)) {
            on_debug_level(n, f);
        }
        return true;
    case ANV_SERVICE_PORT:
        if (assigned && dst == n->id) {
            anv_node_i_on_service(n, src, f);
            return true;
        }
        return false;
    default: return false;
    }
}

/* True if the runtime handled the frame. */
static bool on_command_frame(anv_node_t *n, const anv_link_frame_t *f)
{
    if (n->id == ANV_NODE_UNASSIGNED || anv_id_dst(f->id) != n->id) {
        return false; /* commands are addressed; broadcasts on these ports are other nodes' */
    }
    const uint16_t port = anv_id_port(f->id);
    for (uint8_t i = 0; i < n->n_commands; i++) {
        anv_node_command_t *c = &n->commands[i];
        if (c->msg->port != port) {
            continue;
        }
        if (!from_leader(n, anv_id_src(f->id)) || !anv_node_i_armed(n) || f->len < c->msg->length) {
            n->stats.commands_ignored++;
            return true;
        }
        c->last_ms = n->now_ms;
        if ((c->msg->flags & ANV_MSG_APPLY_SYNC) != 0u) {
            c->held = true;
            c->held_len = f->len;
            memcpy(c->held_data, f->data, f->len);
        } else {
            deliver(n, c, f->data, f->len);
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------------------------ */
/* timers                                                                                      */

static void check_timers(anv_node_t *n)
{
    if (n->have_leader && due(n->now_ms, n->leader_seen_ms + anv_node_i_param_u(n, ANV_PARAM_LEADER_TIMEOUT_MS))) {
        n->stats.leader_losses++;
        lose_leader(n);
    }
    if (anv_node_i_armed(n)) {
        const uint32_t limit = anv_node_i_param_u(n, ANV_PARAM_COMMAND_TIMEOUT_MS);
        for (uint8_t i = 0; i < n->n_commands; i++) {
            const anv_node_command_t *c = &n->commands[i];
            if (c->msg->watchdog_ms == 0u) {
                continue;
            }
            const uint32_t timeout = c->msg->watchdog_ms < limit ? c->msg->watchdog_ms : limit;
            if (due(n->now_ms, c->last_ms + timeout)) {
                n->stats.watchdog_trips++;
                disarm(n);
                break;
            }
        }
    }
    if (n->debug_timed && due(n->now_ms, n->debug_until_ms)) {
        anv_value_t zero = {0};
        (void)anv_node_i_set_param(n, ANV_PARAM_DEBUG_LEVEL, zero, false); /* also ends the timeout */
    }
}

/* ------------------------------------------------------------------------------------------ */
/* lifecycle                                                                                   */

void anv_node_config_default(anv_node_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
}

static bool add_params(anv_node_t *n, const anv_type_desc_t *t)
{
    for (uint16_t i = 0; i < t->n_params; i++) {
        if (n->n_params >= ANV_NODE_MAX_PARAMS) {
            return false;
        }
        n->params[n->n_params++] = &t->params[i];
    }
    return true;
}

bool anv_node_init(anv_node_t *n, const anv_node_config_t *cfg)
{
    memset(n, 0, sizeof *n);
    if (cfg->type == NULL || cfg->link == NULL || cfg->outputs_off == NULL ||
        (cfg->fixed_id != 0u && !valid_id(cfg->fixed_id))) {
        return false;
    }
    n->cfg = *cfg;
    n->state = ANV_NODE_STATE_BOOT;
    n->id = ANV_NODE_UNASSIGNED;
    if (!add_params(n, &anv_protocol_type) || !add_params(n, cfg->type)) {
        return false;
    }
    const anv_type_desc_t *t = cfg->type;
    for (uint16_t i = 0; i < t->n_messages; i++) {
        const anv_msg_desc_t *m = &t->messages[i];
        if (m->kind == ANV_KIND_STREAM && m->sender == ANV_SENDER_NODE) {
            if (n->n_streams >= ANV_NODE_MAX_STREAMS) {
                return false;
            }
            n->streams[n->n_streams++].msg = m;
        } else if (m->kind == ANV_KIND_COMMAND) {
            if (n->n_commands >= ANV_NODE_MAX_COMMANDS) {
                return false;
            }
            n->commands[n->n_commands++].msg = m;
        }
    }
    anv_node_i_load_defaults(n);
    anv_node_i_load_saved(n);

    if (cfg->fixed_id != 0u) {
        n->id = cfg->fixed_id;
    } else if (cfg->storage != NULL) {
        uint8_t id = 0;
        if (cfg->storage->load(cfg->storage_ctx, ANV_NODE_KEY_ID, &id, 1u) == 1u && valid_id(id)) {
            n->id = id;
            n->id_saved = true;
        }
    }
    n->rng = 0x9E3779B9u;
    for (unsigned i = 0; i < 6u; i++) {
        n->rng = (n->rng ^ cfg->mac[i]) * 16777619u;
    }
    if (n->rng == 0u) {
        n->rng = 1u;
    }
    return true;
}

void anv_node_begin(anv_node_t *n, uint32_t now_ms)
{
    n->now_ms = now_ms;
    n->start_ms = now_ms;
    n->cfg.outputs_off(n, n->cfg.user); /* off by default, whatever the hardware came up as */
    n->begun = true;
    use_id(n, n->id);
    if (n->cfg.setup != NULL) {
        n->cfg.setup(n, n->cfg.user);
    }
}

void anv_node_poll(anv_node_t *n, uint32_t now_ms)
{
    if (!n->begun) {
        return;
    }
    n->now_ms = now_ms;
    (void)anv_link_poll(n->cfg.link, now_ms);

    anv_link_frame_t f;
    while (anv_link_receive(n->cfg.link, &f)) {
        const uint16_t port = anv_id_port(f.id);
        const bool handled = port <= ANV_PROTOCOL_LAST_PORT ? on_protocol_frame(n, &f) : on_command_frame(n, &f);
        if (!handled && n->cfg.on_frame != NULL) {
            n->cfg.on_frame(n, &f, n->cfg.user);
        }
    }

    check_timers(n);

    if (n->id == ANV_NODE_UNASSIGNED) {
        if (due(now_ms, n->id_request_next_ms)) {
            send_id_request(n);
            n->id_request_next_ms = now_ms + ANV_NODE_ID_REQUEST_EVERY_MS +
                                    next_random(n) % (ANV_NODE_ID_REQUEST_JITTER_MS / 2u);
        }
    } else {
        if (due(now_ms, n->heartbeat_next_ms)) {
            const uint32_t hz = anv_node_i_param_u(n, ANV_PARAM_HEARTBEAT_HZ);
            n->heartbeat_next_ms = now_ms + 1000u / (hz == 0u ? 1u : hz);
            send_heartbeat(n);
        }
        run_streams(n);
    }

    if (n->cfg.loop != NULL) {
        n->cfg.loop(n, now_ms, n->cfg.user);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* getters                                                                                     */

uint8_t anv_node_state(const anv_node_t *n)
{
    return n->state;
}

bool anv_node_armed(const anv_node_t *n)
{
    return anv_node_i_armed(n);
}

anv_node_id_t anv_node_id(const anv_node_t *n)
{
    return n->id;
}

bool anv_node_has_leader(const anv_node_t *n)
{
    return n->have_leader;
}

anv_node_id_t anv_node_leader(const anv_node_t *n)
{
    return n->leader;
}

const anv_node_stats_t *anv_node_stats(const anv_node_t *n)
{
    return &n->stats;
}

const char *anv_node_state_name(uint8_t state)
{
    switch (state) {
    case ANV_NODE_STATE_BOOT: return "boot";
    case ANV_NODE_STATE_UNASSIGNED: return "unassigned";
    case ANV_NODE_STATE_IDLE: return "idle";
    case ANV_NODE_STATE_ARMED: return "armed";
    case ANV_NODE_STATE_FAULT: return "fault";
    case ANV_NODE_STATE_ESTOP: return "estop";
    case ANV_NODE_STATE_UPDATING: return "updating";
    default: return "?";
    }
}
