"""Rules that span items: uniqueness, ranges, overlaps. Reports every problem, not just one."""
from __future__ import annotations

from .model import Field, Message, NodeType, Param, Schema

COMMON_PARAM_FIRST = 0xF000          # protocol (every-node) parameters live here and above
TYPE_EVENT_RANGE = (0x4000, 0x7FFF)  # node types define their own events here
HOST_TYPE_ID = 1                     # reserved: hosts
MAX_TYPE_ID = 0xFFFF


def validate(schema: Schema) -> list[str]:
    problems: list[str] = []
    problems += _check_type(schema, schema.protocol, is_protocol=True)
    seen_ids: dict[int, str] = {}
    for t in schema.types.values():
        problems += _check_type(schema, t, is_protocol=False)
        if t.type_id in seen_ids:
            problems.append(f"{t.source}: type_id {t.type_id} already used by "
                            f"{seen_ids[t.type_id]}")
        seen_ids[t.type_id] = t.name
        if t.name in RESERVED_TYPE_NAMES or not _ident(t.name):
            problems.append(f"{t.source}: type name {t.name!r} is reserved or not a valid name")
        # Generated C names are anv_<type>_<message>; protocol ones are anv_<message>.
        proto_names = {m.name for m in schema.protocol.messages} | set(schema.protocol.enums)
        for m in t.messages:
            if f"{t.name}_{m.name}" in proto_names:
                problems.append(f"{t.source}: message {m.name} would generate the same C name as "
                                f"protocol message {t.name}_{m.name}")
        common = {p.name for p in schema.protocol.params}
        for p in t.params:
            if p.name in common:
                problems.append(f"{t.source}: param {p.name} clashes with a protocol parameter")
    return problems


def check(schema: Schema) -> None:
    problems = validate(schema)
    if problems:
        raise ValueError("schema problems:\n  " + "\n  ".join(problems))


C_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
    "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
    "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
    "union", "unsigned", "void", "volatile", "while", "bool", "true", "false", "class", "new",
    "delete", "this", "template", "namespace", "private", "public", "operator",
}
RESERVED_TYPE_NAMES = {"protocol", "all", "all_types", "schema", "id", "node", "bits"}


def _ident(name: str) -> bool:
    """A lower_case identifier that is also safe as a C/C++ name."""
    return name.isidentifier() and name == name.lower() and name not in C_KEYWORDS


def _dupes(items: list, key) -> list:
    seen, out = set(), []
    for it in items:
        k = key(it)
        if k in seen:
            out.append(k)
        seen.add(k)
    return out


def _check_type(schema: Schema, t: NodeType, *, is_protocol: bool) -> list[str]:
    where = t.source or t.name
    out: list[str] = []
    if is_protocol:
        if t.type_id != 0:
            out.append(f"{where}: the protocol's type_id must be 0")
    elif not (2 <= t.type_id <= MAX_TYPE_ID):
        out.append(f"{where}: type_id must be 2..{MAX_TYPE_ID} (0 = protocol, 1 = host)")
    if t.version < 1:
        out.append(f"{where}: version must be at least 1")

    # messages
    for name in _dupes(t.messages, lambda m: m.name):
        out.append(f"{where}: two messages named {name}")
    for port in _dupes(t.messages, lambda m: m.port):
        out.append(f"{where}: two messages on port {port}")
    lo, hi = (0, schema.protocol_last_port) if is_protocol else (schema.protocol_last_port + 1,
                                                                  schema.max_port)
    max_prio = (1 << schema.id_layout.get("priority").bits) - 1
    for m in t.messages:
        mw = f"{where}: message {m.name}"
        if not _ident(m.name):
            out.append(f"{mw}: name must be a lower_case identifier")
        if not lo <= m.port <= hi:
            out.append(f"{mw}: port {m.port} outside {lo}..{hi}")
        if not 0 <= m.priority <= max_prio:
            out.append(f"{mw}: priority {m.priority} outside 0..{max_prio}")
        out += _check_message(m, mw)

    # parameters
    for name in _dupes(t.params, lambda p: p.name):
        out.append(f"{where}: two parameters named {name}")
    for idx in _dupes(t.params, lambda p: p.index):
        out.append(f"{where}: two parameters at index {idx:#06x}")
    for p in t.params:
        pw = f"{where}: param {p.name}"
        if not _ident(p.name):
            out.append(f"{pw}: name must be a lower_case identifier")
        if is_protocol and p.index < COMMON_PARAM_FIRST:
            out.append(f"{pw}: protocol parameters use index {COMMON_PARAM_FIRST:#06x} and up")
        if not is_protocol and not 0 <= p.index < COMMON_PARAM_FIRST:
            out.append(f"{pw}: node-type parameters use index 0..{COMMON_PARAM_FIRST - 1:#06x}")
        out += _check_param(p, pw)

    # actions
    for name in _dupes(t.actions, lambda a: a.name):
        out.append(f"{where}: two actions named {name}")
    for idx in _dupes(t.actions, lambda a: a.index):
        out.append(f"{where}: two actions at index {idx}")
    for a in t.actions:
        if not _ident(a.name):
            out.append(f"{where}: action {a.name}: name must be a lower_case identifier")
        if not 0 <= a.index <= 0xFFFF:
            out.append(f"{where}: action {a.name}: index must fit 16 bits")

    # events
    for name in _dupes(t.events, lambda e: e.name):
        out.append(f"{where}: two events named {name}")
    for code in _dupes(t.events, lambda e: e.code):
        out.append(f"{where}: two events with code {code:#06x}")
    for e in t.events:
        in_type_range = TYPE_EVENT_RANGE[0] <= e.code <= TYPE_EVENT_RANGE[1]
        if not _ident(e.name):
            out.append(f"{where}: event {e.name}: name must be a lower_case identifier")
        if not 0 < e.code <= 0xFFFF:
            out.append(f"{where}: event {e.name}: code must be 1..0xFFFF")
        elif is_protocol and in_type_range:
            out.append(f"{where}: event {e.name}: codes {TYPE_EVENT_RANGE[0]:#06x}-"
                       f"{TYPE_EVENT_RANGE[1]:#06x} belong to node types")
        elif not is_protocol and not in_type_range:
            out.append(f"{where}: event {e.name}: node-type events use codes "
                       f"{TYPE_EVENT_RANGE[0]:#06x}-{TYPE_EVENT_RANGE[1]:#06x}")
    return out


def _check_message(m: Message, mw: str) -> list[str]:
    out: list[str] = []
    if m.kind == "frame" and m.fields:
        out.append(f"{mw}: a raw frame has no fields")
    if m.kind == "command" and m.sender == "node":
        out.append(f"{mw}: commands are sent by hosts (sender leader, host or any)")
    if m.kind != "command" and (m.watchdog_ms or m.apply != "now"):
        out.append(f"{mw}: watchdog_ms and apply only make sense on commands")
    if m.watchdog_ms < 0:
        out.append(f"{mw}: watchdog_ms can't be negative")
    if m.rate is not None:
        r = m.rate
        if not (0 <= r.min <= r.max) or not (r.default == 0 or r.min <= r.default <= r.max):
            out.append(f"{mw}: rate needs min <= default <= max (default 0 = off)")
        if m.kind == "stream" and r.max <= 0:
            out.append(f"{mw}: a stream's max rate must be above 0")
    for name in _dupes(m.fields, lambda f: f.name):
        out.append(f"{mw}: two fields named {name}")
    spans = sorted((f.start_bit, f.start_bit + f.bits, f.name) for f in m.fields)
    for (a0, a1, an), (b0, _b1, bn) in zip(spans, spans[1:], strict=False):
        if b0 < a1:
            out.append(f"{mw}: fields {an} and {bn} overlap (bits {a0}..{a1 - 1} and {b0}..)")
    for f in m.fields:
        out += _check_field(f, f"{mw}: field {f.name}")
    return out


def _raw_range(f: Field) -> tuple[float, float]:
    return f.type.raw_min, f.type.raw_max


def _check_field(f: Field, fw: str) -> list[str]:
    out: list[str] = []
    if not _ident(f.name):
        out.append(f"{fw}: name must be a lower_case identifier")
    if f.scale == 0:
        out.append(f"{fw}: scale can't be 0")
    if f.type.is_float:
        if f.enum is not None or f.const is not None:
            out.append(f"{fw}: f32 fields can't be enums or constants")
        return out
    lo, hi = _raw_range(f)
    if f.const is not None and not lo <= f.const <= hi:
        out.append(f"{fw}: const {f.const} doesn't fit {f.type.name}")
    if f.enum is not None:
        bad = [k for k, v in f.enum.values.items() if not lo <= v <= hi]
        if bad:
            out.append(f"{fw}: enum value(s) {', '.join(bad)} don't fit {f.type.name}")
    phys = sorted((lo * f.scale + f.offset, hi * f.scale + f.offset))
    slack = abs(f.scale) / 2
    for label, v in (("min", f.min), ("max", f.max)):
        if v is not None and not phys[0] - slack <= v <= phys[1] + slack:
            out.append(f"{fw}: {label} {v} is outside what {f.type.name} with scale {f.scale} "
                       f"and offset {f.offset} can carry ({phys[0]:g}..{phys[1]:g})")
    if f.min is not None and f.max is not None and f.min > f.max:
        out.append(f"{fw}: min is above max")
    return out


def _check_param(p: Param, pw: str) -> list[str]:
    out: list[str] = []
    if p.min is not None and p.max is not None and p.min > p.max:
        out.append(f"{pw}: min is above max")
    if p.min is not None and p.default < p.min or p.max is not None and p.default > p.max:
        out.append(f"{pw}: default {p.default:g} is outside min/max")
    if not p.type.is_float:
        lo, hi = p.type.raw_min, p.type.raw_max
        for label, v in (("default", p.default), ("min", p.min), ("max", p.max)):
            if v is None:
                continue
            if v != int(v):
                out.append(f"{pw}: {label} {v} must be a whole number for {p.type.name}")
            elif not lo <= v <= hi:
                out.append(f"{pw}: {label} {v:g} doesn't fit {p.type.name}")
    if p.enum is not None and int(p.default) not in p.enum.values.values():
        out.append(f"{pw}: default {p.default:g} isn't one of the enum's values")
    return out
