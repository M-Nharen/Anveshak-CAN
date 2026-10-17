"""Read the YAML schema (schema/protocol.yaml, schema/types/**/*.yaml) into `model` objects.

The loader does three jobs: structure (types, defaults, unknown keys are errors), expansion
(`include`, `count` on parameters, enum references) and layout (bit positions of fields).
Rules that compare things against each other (duplicate ports, ranges) live in validate.py,
so that every problem in a file can be reported at once.
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Any

import yaml

from .hashing import wire_hash
from .model import (
    KINDS,
    PARAM_TYPES,
    SENDERS,
    SEVERITIES,
    Action,
    Enum,
    Event,
    Field,
    IdField,
    IdLayout,
    Message,
    NodeIds,
    NodeType,
    Param,
    Rate,
    Schema,
    parse_value_type,
)

ID_BITS = 29
FRAME_BITS = 64

TYPE_KEYS = {"name", "type_id", "version", "description", "include", "enums", "messages",
             "params", "actions", "events"}
PROTOCOL_KEYS = TYPE_KEYS | {"id_layout", "node_ids", "ports"}
MESSAGE_KEYS = {"name", "port", "priority", "sender", "kind", "fields", "length", "rate",
                "watchdog_ms", "apply", "description"}
FIELD_KEYS = {"name", "type", "count", "scale", "offset", "unit", "min", "max", "enum", "const",
              "start_bit", "description"}
PARAM_KEYS = {"index", "name", "type", "default", "min", "max", "unit", "access", "persist",
              "disarmed_only", "reboot_required", "group", "enum", "count", "description"}
ACTION_KEYS = {"index", "name", "arg", "disarmed_only", "description"}
ARG_KEYS = {"type", "min", "max", "unit"}
EVENT_KEYS = {"code", "name", "severity", "description", "cause"}
INCLUDE_KEYS = {"file", "prefix", "port_offset", "index_offset", "action_offset", "event_offset"}

DEFAULT_PRIORITY = {"command": 1, "stream": 2, "event": 3, "frame": 6}
SLOW_STREAM_HZ = 20          # streams slower than this default to priority 3


class SchemaError(Exception):
    """A problem in a schema file. `where` says which file and which item."""

    def __init__(self, where: str, message: str):
        super().__init__(f"{where}: {message}")
        self.where = where
        self.message = message


def default_schema_dir() -> Path:
    """$ANV_SCHEMA_DIR, else the `schema/` folder of the repo this package sits in."""
    env = os.environ.get("ANV_SCHEMA_DIR")
    if env:
        return Path(env)
    here = Path(__file__).resolve()
    for parent in here.parents:
        if (parent / "schema" / "protocol.yaml").is_file():
            return parent / "schema"
    raise FileNotFoundError("schema directory not found; set ANV_SCHEMA_DIR")


def load(schema_dir: str | Path | None = None, *, types: list[str] | None = None) -> Schema:
    """Load protocol.yaml and every file under types/ (or only the named types)."""
    root = Path(schema_dir) if schema_dir is not None else default_schema_dir()
    proto_path = root / "protocol.yaml"
    raw = _read_yaml(proto_path)
    _check_keys(raw, PROTOCOL_KEYS, str(proto_path))

    layout = _parse_id_layout(raw.get("id_layout"), str(proto_path))
    ids = _parse_node_ids(raw.get("node_ids", {}), str(proto_path))
    ports = raw.get("ports", {})
    protocol_last = int(ports.get("protocol_last", 63))

    protocol = _parse_type(raw, proto_path, root, enums_from=None)
    schema = Schema(protocol=protocol, id_layout=layout, node_ids=ids,
                    protocol_last_port=protocol_last)

    for path in sorted((root / "types").rglob("*.yaml")):
        t = _parse_type(_read_yaml(path), path, root, enums_from=protocol)
        if types is not None and t.name not in types:
            continue
        if t.name in schema.types:
            raise SchemaError(str(path), f"type name {t.name!r} already used by "
                              f"{schema.types[t.name].source}")
        schema.types[t.name] = t
    if types is not None:
        missing = set(types) - set(schema.types)
        if missing:
            raise SchemaError(str(root), f"no such node type(s): {', '.join(sorted(missing))}")
    return schema


def load_type_file(path: str | Path, schema_dir: str | Path | None = None) -> NodeType:
    """Load one node-type file on its own (protocol enums are still available to it)."""
    root = Path(schema_dir) if schema_dir is not None else default_schema_dir()
    protocol = _parse_type(_read_yaml(root / "protocol.yaml"), root / "protocol.yaml", root,
                           enums_from=None)
    return _parse_type(_read_yaml(Path(path)), Path(path), root, enums_from=protocol)


# --- files ------------------------------------------------------------------------------

def _read_yaml(path: Path) -> dict[str, Any]:
    try:
        with open(path, encoding="utf-8") as f:
            data = yaml.safe_load(f)
    except yaml.YAMLError as e:
        raise SchemaError(str(path), f"not valid YAML: {e}") from e
    except OSError as e:
        raise SchemaError(str(path), str(e)) from e
    if not isinstance(data, dict):
        raise SchemaError(str(path), "expected a mapping at the top level")
    return data


def _check_keys(item: dict[str, Any], allowed: set[str], where: str) -> None:
    if not isinstance(item, dict):
        raise SchemaError(where, f"expected a mapping, got {type(item).__name__}")
    unknown = set(item) - allowed
    if unknown:
        raise SchemaError(where, f"unknown key(s) {', '.join(sorted(unknown))}; "
                          f"allowed: {', '.join(sorted(allowed))}")


def _require(item: dict[str, Any], key: str, where: str) -> Any:
    if key not in item:
        raise SchemaError(where, f"missing {key!r}")
    return item[key]


# --- id layout --------------------------------------------------------------------------

def _parse_id_layout(raw: Any, where: str) -> IdLayout:
    if not isinstance(raw, list) or not raw:
        raise SchemaError(where, "id_layout must be a list of {field, bits}")
    names = [r.get("field") for r in raw]
    if sorted(names) != ["dst", "port", "priority", "src"]:
        raise SchemaError(where, "id_layout must have exactly priority, dst, port and src")
    if names[0] != "priority":
        raise SchemaError(where, "priority must be the most significant id_layout field")
    total = sum(int(r.get("bits", 0)) for r in raw)
    if total != ID_BITS:
        raise SchemaError(where, f"id_layout bits add up to {total}, not {ID_BITS}")
    fields = []
    shift = ID_BITS
    for r in raw:
        bits = int(r["bits"])
        if bits < 1:
            raise SchemaError(where, f"id_layout field {r['field']} needs at least 1 bit")
        shift -= bits
        fields.append(IdField(r["field"], bits, shift))
    return IdLayout(fields)


def _parse_node_ids(raw: dict[str, Any], where: str) -> NodeIds:
    try:
        return NodeIds(broadcast=int(raw.get("broadcast", 0)),
                       unassigned=int(raw.get("unassigned", 255)),
                       first=int(raw.get("first", 1)), last=int(raw.get("last", 254)))
    except (TypeError, ValueError) as e:
        raise SchemaError(where, f"node_ids: {e}") from e


# --- node types -------------------------------------------------------------------------

def _parse_type(raw: dict[str, Any], path: Path, root: Path, enums_from: NodeType | None
                ) -> NodeType:
    where = str(path)
    _check_keys(raw, PROTOCOL_KEYS if enums_from is None else TYPE_KEYS, where)
    name = _require(raw, "name", where)
    if not isinstance(name, str) or not name.isidentifier() or name != name.lower():
        raise SchemaError(where, f"type name {name!r} must be a lower_case identifier")
    type_id = _require(raw, "type_id", where)
    t = NodeType(name=name, type_id=int(type_id), version=int(raw.get("version", 1)),
                 description=str(raw.get("description", "")).strip(), source=where)

    # Pieces from include files come first, then this file's own, so a type can override
    # nothing silently: duplicates are reported by validate.py.
    sections: dict[str, list[Any]] = {"messages": [], "params": [], "actions": [], "events": []}
    enums_raw: dict[str, Any] = {}
    for inc in _as_list(raw.get("include"), where, "include"):
        _merge_include(inc, path, root, sections, enums_raw, where)
    for key in sections:
        sections[key].extend(_as_list(raw.get(key), where, key))
    enums_raw.update(raw.get("enums") or {})

    base_enums = dict(enums_from.enums) if enums_from is not None else {}
    for ename, values in enums_raw.items():
        t.enums[ename] = _parse_enum(ename, values, f"{where}: enum {ename}")
    lookup = {**base_enums, **t.enums}

    t.messages = [_parse_message(m, lookup, f"{where}: message {m.get('name', '?')}")
                  for m in sections["messages"]]
    for p in sections["params"]:
        t.params.extend(_parse_params(p, lookup, f"{where}: param {p.get('name', '?')}"))
    t.actions = [_parse_action(a, f"{where}: action {a.get('name', '?')}")
                 for a in sections["actions"]]
    t.events = [_parse_event(e, f"{where}: event {e.get('name', '?')}")
                for e in sections["events"]]
    t.wire_hash = wire_hash(t)
    return t


def _as_list(value: Any, where: str, key: str) -> list[Any]:
    if value is None:
        return []
    if not isinstance(value, list):
        raise SchemaError(where, f"{key} must be a list")
    return value


def _merge_include(inc: Any, path: Path, root: Path, sections: dict[str, list[Any]],
                   enums_raw: dict[str, Any], where: str) -> None:
    """Pull a fragment in, optionally renamed (`prefix`) and moved (`*_offset`).

    The same fragment can be included twice (two power monitors, say) by giving each
    copy its own prefix and offsets.
    """
    spec = {"file": inc} if isinstance(inc, str) else inc
    _check_keys(spec, INCLUDE_KEYS, f"{where}: include")
    rel = _require(spec, "file", f"{where}: include")
    candidates = [path.parent / rel, root / rel, root / "fragments" / rel]
    frag_path = next((c for c in candidates if c.is_file()), None)
    if frag_path is None:
        raise SchemaError(where, f"include {rel!r} not found (looked next to the file, in "
                          f"{root} and in {root / 'fragments'})")
    frag = _read_yaml(frag_path)
    _check_keys(frag, {"description", "enums", "messages", "params", "actions", "events"},
                str(frag_path))
    prefix = spec.get("prefix", "")
    offsets = {"messages": ("port", int(spec.get("port_offset", 0))),
               "params": ("index", int(spec.get("index_offset", 0))),
               "actions": ("index", int(spec.get("action_offset", 0))),
               "events": ("code", int(spec.get("event_offset", 0)))}
    for key, (num_key, offset) in offsets.items():
        for item in _as_list(frag.get(key), str(frag_path), key):
            item = dict(item)
            if prefix and "name" in item:
                item["name"] = f"{prefix}{item['name']}"
            if num_key in item:
                item[num_key] = int(item[num_key]) + offset
            sections[key].append(item)
    for ename, values in (frag.get("enums") or {}).items():
        enums_raw.setdefault(ename, values)


def _parse_enum(name: str, values: Any, where: str) -> Enum:
    if isinstance(values, list):                 # [a, b, c] -> a=0, b=1, c=2
        values = {v: i for i, v in enumerate(values)}
    if not isinstance(values, dict) or not values:
        raise SchemaError(where, "an enum is a non-empty mapping label: number (or a list)")
    out: dict[str, int] = {}
    for label, v in values.items():
        if not str(label).isidentifier():
            raise SchemaError(where, f"enum label {label!r} must be an identifier")
        out[str(label)] = int(v)
    return Enum(name, out)


def _resolve_enum(value: Any, lookup: dict[str, Enum], owner: str, where: str) -> Enum | None:
    if value is None:
        return None
    if isinstance(value, str):
        if value not in lookup:
            raise SchemaError(where, f"unknown enum {value!r}")
        return lookup[value]
    return _parse_enum(owner, value, where)       # inline enum


def _parse_message(raw: dict[str, Any], enums: dict[str, Enum], where: str) -> Message:
    _check_keys(raw, MESSAGE_KEYS, where)
    name = _require(raw, "name", where)
    kind = raw.get("kind", "stream")
    if kind not in KINDS:
        raise SchemaError(where, f"kind must be one of {', '.join(KINDS)}")
    sender = raw.get("sender", "leader" if kind == "command" else "node")
    if sender not in SENDERS:
        raise SchemaError(where, f"sender must be one of {', '.join(SENDERS)}")

    rate = None
    if "rate" in raw:
        r = raw["rate"]
        if isinstance(r, (int, float)):
            r = {"default": r}
        if not isinstance(r, dict) or set(r) - {"default", "min", "max"}:
            raise SchemaError(where, "rate is a number (Hz) or {default, min, max}")
        default = float(r.get("default", 0))
        rate = Rate(default=default, min=float(r.get("min", 0)),
                    max=float(r.get("max", max(default, 1000.0))))
    elif kind == "stream":
        raise SchemaError(where, "a stream needs a rate (Hz)")

    if "priority" in raw:
        priority = int(raw["priority"])
    elif kind == "stream" and rate is not None and rate.default < SLOW_STREAM_HZ:
        priority = 3
    else:
        priority = DEFAULT_PRIORITY[kind]

    fields: list[Field] = []
    bit = 0
    for f in _as_list(raw.get("fields"), where, "fields"):
        fld = _parse_field(f, enums, bit, f"{where}: field {f.get('name', '?')}")
        fields.append(fld)
        bit = fld.start_bit + fld.bits
    end = max((f.start_bit + f.bits for f in fields), default=0)
    if end > FRAME_BITS:
        raise SchemaError(where, f"fields need {end} bits; a CAN frame holds {FRAME_BITS}")
    length = int(raw.get("length", (end + 7) // 8 if kind != "frame" else 8))
    if not 0 <= length <= 8 or length * 8 < end:
        raise SchemaError(where, f"length {length} can't hold {end} bits (max 8 bytes)")

    apply = raw.get("apply", "now")
    if apply not in ("now", "sync"):
        raise SchemaError(where, "apply must be 'now' or 'sync'")
    return Message(name=str(name), port=int(_require(raw, "port", where)), priority=priority,
                   sender=sender, kind=kind, fields=fields, length=length, rate=rate,
                   watchdog_ms=int(raw.get("watchdog_ms", 0)), apply=apply,
                   description=_text(raw.get("description")))


def _parse_field(raw: dict[str, Any], enums: dict[str, Enum], next_bit: int, where: str
                 ) -> Field:
    _check_keys(raw, FIELD_KEYS, where)
    name = _require(raw, "name", where)
    try:
        vtype = parse_value_type(str(_require(raw, "type", where)))
    except ValueError as e:
        raise SchemaError(where, str(e)) from e
    start = int(raw.get("start_bit", next_bit))
    count = int(raw.get("count", 1))
    if count < 1:
        raise SchemaError(where, "count must be at least 1")
    return Field(name=str(name), type=vtype, start_bit=start, count=count,
                 scale=float(raw.get("scale", 1.0)), offset=float(raw.get("offset", 0.0)),
                 unit=str(raw.get("unit", "")), min=_opt_float(raw.get("min")),
                 max=_opt_float(raw.get("max")),
                 enum=_resolve_enum(raw.get("enum"), enums, str(name), where),
                 const=None if raw.get("const") is None else int(raw["const"]),
                 description=_text(raw.get("description")))


def _parse_params(raw: dict[str, Any], enums: dict[str, Enum], where: str) -> list[Param]:
    """One YAML entry -> one Param, or `count` of them (name_0, name_1, ... at index, index+1)."""
    _check_keys(raw, PARAM_KEYS, where)
    name = str(_require(raw, "name", where))
    tname = str(_require(raw, "type", where))
    if tname not in PARAM_TYPES:
        raise SchemaError(where, f"parameter type must be one of {', '.join(PARAM_TYPES)}")
    access = raw.get("access", "rw")
    if access not in ("rw", "ro"):
        raise SchemaError(where, "access must be 'rw' or 'ro'")
    count = int(raw.get("count", 1))
    index = int(_require(raw, "index", where))
    vtype = parse_value_type(tname)
    out = []
    for i in range(count):
        out.append(Param(
            index=index + i, name=f"{name}_{i}" if count > 1 else name, type=vtype,
            default=float(raw.get("default", 0)), min=_opt_float(raw.get("min")),
            max=_opt_float(raw.get("max")), unit=str(raw.get("unit", "")), access=access,
            persist=bool(raw.get("persist", access == "rw")),
            disarmed_only=bool(raw.get("disarmed_only", False)),
            reboot_required=bool(raw.get("reboot_required", False)),
            group=str(raw.get("group", name if count > 1 else "")),
            enum=_resolve_enum(raw.get("enum"), enums, name, where),
            description=_text(raw.get("description"))))
    return out


def _parse_action(raw: dict[str, Any], where: str) -> Action:
    _check_keys(raw, ACTION_KEYS, where)
    a = Action(index=int(_require(raw, "index", where)), name=str(_require(raw, "name", where)),
               disarmed_only=bool(raw.get("disarmed_only", False)),
               description=_text(raw.get("description")))
    arg = raw.get("arg")
    if arg is not None:
        if isinstance(arg, str):
            arg = {"type": arg}
        _check_keys(arg, ARG_KEYS, f"{where}: arg")
        tname = str(_require(arg, "type", f"{where}: arg"))
        if tname not in PARAM_TYPES:
            raise SchemaError(where, f"action arg type must be one of {', '.join(PARAM_TYPES)}")
        a.arg = parse_value_type(tname)
        a.arg_min = _opt_float(arg.get("min"))
        a.arg_max = _opt_float(arg.get("max"))
        a.arg_unit = str(arg.get("unit", ""))
    return a


def _parse_event(raw: dict[str, Any], where: str) -> Event:
    _check_keys(raw, EVENT_KEYS, where)
    severity = raw.get("severity", "info")
    if severity not in SEVERITIES:
        raise SchemaError(where, f"severity must be one of {', '.join(SEVERITIES)}")
    return Event(code=int(_require(raw, "code", where)), name=str(_require(raw, "name", where)),
                 severity=severity, description=_text(raw.get("description")),
                 cause=_text(raw.get("cause")))


def _opt_float(v: Any) -> float | None:
    return None if v is None else float(v)


def _text(v: Any) -> str:
    return "" if v is None else " ".join(str(v).split())
