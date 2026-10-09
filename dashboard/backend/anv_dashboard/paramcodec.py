"""Physical value <-> raw wire bits for a SERVICE parameter (defs/params/*.yaml `type`).

The bus only ever carries VALUE_RAW (32 raw bits, reinterpreted per the parameter's type:
defs/rover.dbc comment on *_SERVICE_REQ_*/_RESP_*). u8/u32/i32 are plain integers; f32 is the
IEEE-754 bit pattern, so it needs an explicit reinterpret, not a numeric cast.
"""

from __future__ import annotations

import struct

_U32 = 0xFFFFFFFF


def to_raw(param_type: str, physical: float) -> int:
    if param_type == "f32":
        return struct.unpack("<I", struct.pack("<f", physical))[0]
    if param_type == "i32":
        return struct.unpack("<I", struct.pack("<i", round(physical)))[0]
    if param_type in ("u32", "u8"):
        return round(physical) & _U32
    raise ValueError(f"unknown parameter type {param_type!r}")


def to_physical(param_type: str, raw: int) -> float:
    raw &= _U32
    if param_type == "f32":
        return struct.unpack("<f", struct.pack("<I", raw))[0]
    if param_type == "i32":
        return float(struct.unpack("<i", struct.pack("<I", raw))[0])
    if param_type in ("u32", "u8"):
        return float(raw)
    raise ValueError(f"unknown parameter type {param_type!r}")
