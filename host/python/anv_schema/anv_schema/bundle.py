"""The schema as one JSON file (schema/generated/schema.json).

Programs that shouldn't parse YAML or depend on the source tree (the C++ host library, the
dashboard, a laptop tool) load this instead. Everything is already expanded: field bit
positions, parameter arrays, includes, enum references, wire hashes.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .model import (
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

BUNDLE_FORMAT = 1


def _enum(e: Enum | None) -> Any:
    return None if e is None else {"name": e.name, "values": e.values}


def _type_dict(t: NodeType) -> dict[str, Any]:
    return {
        "name": t.name, "type_id": t.type_id, "version": t.version,
        "description": t.description, "wire_hash": t.wire_hash,
        "enums": {k: e.values for k, e in t.enums.items()},
        "messages": [{
            "name": m.name, "port": m.port, "priority": m.priority, "sender": m.sender,
            "kind": m.kind, "length": m.length, "watchdog_ms": m.watchdog_ms, "apply": m.apply,
            "rate": None if m.rate is None else {"default": m.rate.default, "min": m.rate.min,
                                                 "max": m.rate.max},
            "description": m.description,
            "fields": [{
                "name": f.name, "type": f.type.name, "start_bit": f.start_bit, "count": f.count,
                "scale": f.scale, "offset": f.offset, "unit": f.unit, "min": f.min, "max": f.max,
                "enum": _enum(f.enum), "const": f.const, "description": f.description,
            } for f in m.fields],
        } for m in t.messages],
        "params": [{
            "index": p.index, "name": p.name, "type": p.type.name, "default": p.default,
            "min": p.min, "max": p.max, "unit": p.unit, "access": p.access,
            "persist": p.persist, "disarmed_only": p.disarmed_only,
            "reboot_required": p.reboot_required, "group": p.group, "enum": _enum(p.enum),
            "description": p.description,
        } for p in t.params],
        "actions": [{
            "index": a.index, "name": a.name, "arg": a.arg.name if a.arg else None,
            "arg_min": a.arg_min, "arg_max": a.arg_max, "arg_unit": a.arg_unit,
            "disarmed_only": a.disarmed_only, "description": a.description,
        } for a in t.actions],
        "events": [{"code": e.code, "name": e.name, "severity": e.severity,
                    "description": e.description, "cause": e.cause} for e in t.events],
    }


def to_dict(schema: Schema) -> dict[str, Any]:
    return {
        "format": BUNDLE_FORMAT,
        "id_layout": [{"field": f.name, "bits": f.bits, "shift": f.shift}
                      for f in schema.id_layout.fields],
        "node_ids": vars(schema.node_ids),
        "protocol_last_port": schema.protocol_last_port,
        "protocol": _type_dict(schema.protocol),
        "types": [_type_dict(t) for t in sorted(schema.types.values(), key=lambda t: t.type_id)],
    }


def dumps(schema: Schema) -> str:
    return json.dumps(to_dict(schema), indent=1, sort_keys=False) + "\n"


def _enum_from(d: Any) -> Enum | None:
    return None if d is None else Enum(d["name"], dict(d["values"]))


def _type_from(d: dict[str, Any]) -> NodeType:
    t = NodeType(name=d["name"], type_id=d["type_id"], version=d["version"],
                 description=d["description"], wire_hash=d["wire_hash"], source="bundle")
    t.enums = {k: Enum(k, dict(v)) for k, v in d["enums"].items()}
    for m in d["messages"]:
        r = m["rate"]
        t.messages.append(Message(
            name=m["name"], port=m["port"], priority=m["priority"], sender=m["sender"],
            kind=m["kind"], length=m["length"], watchdog_ms=m["watchdog_ms"], apply=m["apply"],
            rate=None if r is None else Rate(r["default"], r["min"], r["max"]),
            description=m["description"],
            fields=[Field(name=f["name"], type=parse_value_type(f["type"]),
                          start_bit=f["start_bit"], count=f["count"], scale=f["scale"],
                          offset=f["offset"], unit=f["unit"], min=f["min"], max=f["max"],
                          enum=_enum_from(f["enum"]), const=f["const"],
                          description=f["description"]) for f in m["fields"]]))
    for p in d["params"]:
        t.params.append(Param(
            index=p["index"], name=p["name"], type=parse_value_type(p["type"]),
            default=p["default"], min=p["min"], max=p["max"], unit=p["unit"],
            access=p["access"], persist=p["persist"], disarmed_only=p["disarmed_only"],
            reboot_required=p["reboot_required"], group=p["group"], enum=_enum_from(p["enum"]),
            description=p["description"]))
    for a in d["actions"]:
        t.actions.append(Action(
            index=a["index"], name=a["name"],
            arg=parse_value_type(a["arg"]) if a["arg"] else None, arg_min=a["arg_min"],
            arg_max=a["arg_max"], arg_unit=a["arg_unit"], disarmed_only=a["disarmed_only"],
            description=a["description"]))
    t.events = [Event(e["code"], e["name"], e["severity"], e["description"], e["cause"])
                for e in d["events"]]
    return t


def from_dict(d: dict[str, Any]) -> Schema:
    if d.get("format") != BUNDLE_FORMAT:
        raise ValueError(f"bundle format {d.get('format')!r}, this code reads {BUNDLE_FORMAT}")
    layout = IdLayout([IdField(f["field"], f["bits"], f["shift"]) for f in d["id_layout"]])
    s = Schema(protocol=_type_from(d["protocol"]), id_layout=layout,
               node_ids=NodeIds(**d["node_ids"]), protocol_last_port=d["protocol_last_port"])
    for td in d["types"]:
        t = _type_from(td)
        s.types[t.name] = t
    return s


def load_bundle(path: str | Path) -> Schema:
    with open(path, encoding="utf-8") as f:
        return from_dict(json.load(f))
