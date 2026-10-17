"""Turn message values into the 0-8 bytes of a CAN frame and back.

Bit layout: the frame is one little-endian integer; a field occupying bits [s, s+n) is
`(frame >> s) & ((1 << n) - 1)`. This is DBC's "Intel" byte order, so SavvyCAN and
cantools read the same values. The C generator emits code with exactly the same rule.

Values are physical (scaled, with enum labels) by default; pass raw=True to work in the
integers that go on the wire.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass
from typing import Any

from .model import Field, Message, NodeType, Schema


class CodecError(ValueError):
    pass


def _to_raw(f: Field, value: Any, raw: bool, strict: bool) -> int:
    t = f.type
    if t.is_float:
        return struct.unpack("<I", struct.pack("<f", float(value)))[0]
    if isinstance(value, str):
        if f.enum is None or value not in f.enum.values:
            raise CodecError(f"{f.name}: {value!r} is not a label of this field")
        n = f.enum.values[value]
    elif isinstance(value, bool):
        n = int(value)
    elif raw or not f.is_scaled:
        if isinstance(value, float) and not value.is_integer():
            if raw:
                raise CodecError(f"{f.name}: raw value {value} must be an integer")
            value = round(value)
        n = int(value)
    else:
        if strict and ((f.min is not None and value < f.min) or
                       (f.max is not None and value > f.max)):
            raise CodecError(f"{f.name}: {value} outside {f.min}..{f.max} {f.unit}".rstrip())
        n = round((float(value) - f.offset) / f.scale)
    if not t.raw_min <= n <= t.raw_max:
        raise CodecError(f"{f.name}: {value} doesn't fit {t.name} "
                         f"(raw {n}, allowed {t.raw_min}..{t.raw_max})")
    return n & ((1 << t.bits) - 1)


def _from_raw(f: Field, bits: int, raw: bool, labels: bool) -> Any:
    t = f.type
    if t.is_float:
        return struct.unpack("<f", struct.pack("<I", bits))[0]
    n = bits
    if t.signed and n & (1 << (t.bits - 1)):
        n -= 1 << t.bits
    if t.is_bool:
        return bool(n)
    if raw:
        return n
    if f.enum is not None and labels:
        label = f.enum.label(n)
        return label if label is not None else n
    if f.is_scaled:
        return n * f.scale + f.offset
    return n


def encode(msg: Message, values: dict[str, Any] | None = None, *, raw: bool = False,
           strict: bool = True) -> bytes:
    """values: field name -> value (a list for arrays). Missing fields are 0 (or their const).

    strict=True refuses unknown field names and physical values outside a field's min/max.
    """
    values = values or {}
    if strict:
        unknown = set(values) - {f.name for f in msg.fields}
        if unknown:
            raise CodecError(f"{msg.name} has no field(s) {', '.join(sorted(unknown))}")
    frame = 0
    for f in msg.fields:
        v = f.const if f.const is not None else values.get(f.name, 0)
        items = v if f.count > 1 else [v]
        if f.count > 1:
            if not isinstance(items, (list, tuple)):
                items = [items] * f.count
            if len(items) != f.count:
                raise CodecError(f"{f.name}: needs {f.count} values, got {len(items)}")
        for i, item in enumerate(items):
            frame |= _to_raw(f, item, raw, strict) << f.element_start(i)
    return frame.to_bytes(8, "little")[: msg.length]


def decode(msg: Message, data: bytes, *, raw: bool = False, labels: bool = True,
           strict: bool = True) -> dict[str, Any]:
    """bytes -> {field: value}. strict=True checks the length and constant fields."""
    if strict and len(data) < msg.length:
        raise CodecError(f"{msg.name}: {len(data)} bytes, needs {msg.length}")
    frame = int.from_bytes(bytes(data[:8]).ljust(8, b"\0"), "little")
    out: dict[str, Any] = {}
    for f in msg.fields:
        mask = (1 << f.type.bits) - 1
        vals = [_from_raw(f, (frame >> f.element_start(i)) & mask, raw, labels)
                for i in range(f.count)]
        out[f.name] = vals if f.count > 1 else vals[0]
        if strict and f.const is not None and ((frame >> f.start_bit) & mask) != f.const:
            raise CodecError(f"{msg.name}: field {f.name} should be {f.const:#x}")
    return out


@dataclass
class DecodedFrame:
    can_id: int
    priority: int
    src: int
    dst: int
    port: int
    message: Message | None      # None when no schema describes this port for this sender
    values: dict[str, Any] | None
    error: str | None = None


def decode_frame(schema: Schema, can_id: int, data: bytes,
                 node_type: NodeType | None = None, **kwargs: Any) -> DecodedFrame:
    """Split the ID, find the message (protocol, or the sender's type) and decode it.

    node_type is the type of the node that sent the frame (src), when known. Frames on
    node-type ports sent by a host to a node are looked up in the destination's type, so
    pass that type instead when decoding commands.
    """
    parts = schema.id_layout.parse(can_id)
    msg = schema.message_for(parts["port"], node_type)
    values, error = None, None
    if msg is not None and msg.kind != "frame":
        try:
            values = decode(msg, data, **kwargs)
        except CodecError as e:
            error = str(e)
    return DecodedFrame(can_id=can_id, priority=parts["priority"], src=parts["src"],
                        dst=parts["dst"], port=parts["port"], message=msg, values=values,
                        error=error)


def make_id(schema: Schema, msg: Message, *, src: int, dst: int = 0,
            priority: int | None = None) -> int:
    """The 29-bit ID for `msg` from src to dst (0 = broadcast), at the message's priority."""
    return schema.id_layout.make(priority=msg.priority if priority is None else priority,
                                 dst=dst, port=msg.port, src=src)


def physical_range(f: Field) -> tuple[float, float]:
    """The physical values a field can carry (ignoring its min/max)."""
    if f.type.is_float:
        return (-math.inf, math.inf)
    a = f.type.raw_min * f.scale + f.offset
    b = f.type.raw_max * f.scale + f.offset
    return (min(a, b), max(a, b))
