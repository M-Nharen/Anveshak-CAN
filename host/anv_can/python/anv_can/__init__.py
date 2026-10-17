"""Anveshak CAN host library: the C++ anv_link + anv_proto, from Python.

    import anv_can
    s = anv_can.load_schema()
    m = s.type("motor_controller").message("setpoint")
    data = anv_can.encode(m, {"mode": "velocity", "target": [1.5, -2.0]})
    frame = anv_can.Frame(anv_can.make_id(s, m, src=1, dst=20), data)
    link = anv_can.open_socketcan("vcan0")          # or anv_can.TestBus().connect()
    link.send(frame)

Values are physical (scaled, enum labels) by default; raw=True gives the wire integers, the
same as anv_schema's Python codec, and the bytes match the ESP32's generated C exactly.

Host (anv_host) is a brain on the bus: discovery, ID assignment, leader election and the
clients for settings, stream rates, commands, actions, arm/disarm and E-stop:

    host = anv_can.Host(link, s, host_id=1)
    host.set_callbacks(node_appeared=lambda n, returned: print(n))
    host.start()
    host.wait_for_node(20, 2000)
    host.set(20, "heartbeat_hz", 20)
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ._anv_can import (  # noqa: F401
    CodecError,
    Field,
    Frame,
    Host,
    HostError,
    IsoTpChannel,
    IsoTpError,
    Link,
    Message,
    NodeInfo,
    NodeType,
    PeerHost,
    Schema,
    TestBus,
    decode,
    encode,
)

try:
    from ._anv_can import open_socketcan  # noqa: F401  (Linux only)
except ImportError:  # pragma: no cover
    pass

_BUNDLED = Path(__file__).with_name("schema.json")


def schema_path(path: str | os.PathLike[str] | None = None) -> Path:
    """Where the schema comes from: the argument, $ANV_SCHEMA_JSON, the copy installed with
    this package, then the source tree it was built from."""
    for candidate in (path, os.environ.get("ANV_SCHEMA_JSON"), _BUNDLED, Schema.default_path()):
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    raise FileNotFoundError("no schema.json: pass a path or set ANV_SCHEMA_JSON")


def load_schema(path: str | os.PathLike[str] | None = None) -> Schema:
    return Schema.load(str(schema_path(path)))


def make_id(schema: Schema, msg: Message, *, src: int, dst: int = 0,
            priority: int | None = None) -> int:
    """The 29-bit ID for msg from src to dst (0 = broadcast), at the message's priority."""
    return schema.make_id(msg.priority if priority is None else priority, dst, msg.port, src)


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


def decode_frame(schema: Schema, can_id: int, data: bytes, node_type: NodeType | None = None,
                 **kwargs: Any) -> DecodedFrame:
    """Split the ID, find the message (protocol, or node_type's) and decode it.

    node_type is the sender's type, or for a command the receiver's. kwargs go to decode().
    """
    p = schema.parse_id(can_id)
    msg = schema.message_for(p["port"], node_type)
    values, error = None, None
    if msg is not None and msg.kind != "frame":
        try:
            values = decode(msg, bytes(data), **kwargs)
        except CodecError as e:
            error = str(e)
    return DecodedFrame(can_id, p["priority"], p["src"], p["dst"], p["port"], msg, values, error)


def enum_label(schema: Schema, enum: str, value: int) -> str:
    """A protocol enum's label for value (e.g. node_state 3 -> "armed"), or the number."""
    for label, number in schema.protocol.enums.get(enum, {}).items():
        if number == value:
            return label
    return str(value)
