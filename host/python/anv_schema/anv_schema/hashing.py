"""The wire hash: one number that changes whenever two sides would stop understanding each other.

It covers what goes on the wire (ports, layouts, scaling, parameter and action indices and
types, event codes, the type's ID and version) and leaves out what doesn't (descriptions,
units, defaults, limits). A node reports the low 16 bits in its heartbeat; a host compares
them with its own schema and flags a mismatch instead of decoding garbage.
"""
from __future__ import annotations

import json
import zlib
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .model import NodeType


def _wire_view(t: NodeType) -> dict[str, Any]:
    def enum_view(e):
        return None if e is None else sorted(e.values.items())

    return {
        "type_id": t.type_id,
        "version": t.version,
        "messages": sorted(
            [m.port, m.name, m.priority, m.sender, m.kind, m.length, m.apply,
             [[f.name, f.type.name, f.start_bit, f.count, f.scale, f.offset, f.const,
               enum_view(f.enum)] for f in m.fields]]
            for m in t.messages),
        "params": sorted([p.index, p.name, p.type.name, p.access, enum_view(p.enum)]
                         for p in t.params),
        "actions": sorted([a.index, a.name, a.arg.name if a.arg else None] for a in t.actions),
        "events": sorted([e.code, e.name] for e in t.events),
    }


def wire_hash(t: NodeType) -> int:
    text = json.dumps(_wire_view(t), sort_keys=True, separators=(",", ":"))
    return zlib.crc32(text.encode()) & 0xFFFFFFFF
