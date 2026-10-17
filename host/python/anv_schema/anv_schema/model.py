"""The schema as plain data: what a node type is made of.

Everything here is produced by `loader.py` from YAML (or `bundle.py` from JSON) and consumed
by the codec and the generators. Nothing in this module knows about any particular rover.
"""
from __future__ import annotations

from dataclasses import dataclass, field

# Wire value types. `uN` / `iN` (N = 1..64, i needs N >= 2) are unsigned / two's-complement
# integers of N bits; `bool` is one bit; `f32` is an IEEE-754 float. Parameters are limited
# to the types that fit a 32-bit service value: PARAM_TYPES.
PARAM_TYPES = ("bool", "u8", "i8", "u16", "i16", "u32", "i32", "f32")

SENDERS = ("node", "host", "leader", "any")
KINDS = ("stream", "event", "command", "frame")
SEVERITIES = ("info", "warning", "error", "critical")


@dataclass(frozen=True)
class ValueType:
    """A wire type: how many bits and how to read them."""

    name: str
    bits: int
    signed: bool
    is_float: bool = False

    @property
    def is_bool(self) -> bool:
        return self.name == "bool"

    @property
    def raw_min(self) -> int:
        if self.is_float:
            raise ValueError("float has no integer range")
        return -(1 << (self.bits - 1)) if self.signed else 0

    @property
    def raw_max(self) -> int:
        if self.is_float:
            raise ValueError("float has no integer range")
        return (1 << (self.bits - 1)) - 1 if self.signed else (1 << self.bits) - 1

    @property
    def c_type(self) -> str:
        if self.is_float:
            return "float"
        if self.is_bool:
            return "bool"
        for width in (8, 16, 32, 64):
            if self.bits <= width:
                return f"{'int' if self.signed else 'uint'}{width}_t"
        raise ValueError(self.name)


def parse_value_type(name: str) -> ValueType:
    """'u12' -> ValueType('u12', 12, False). Raises ValueError for anything else."""
    if name == "bool":
        return ValueType("bool", 1, False)
    if name == "f32":
        return ValueType("f32", 32, True, is_float=True)
    if len(name) >= 2 and name[0] in "ui" and name[1:].isdigit():
        bits = int(name[1:])
        signed = name[0] == "i"
        if 1 <= bits <= 64 and not (signed and bits < 2):
            return ValueType(name, bits, signed)
    raise ValueError(f"unknown type {name!r} (use bool, f32, u1..u64 or i2..i64)")


@dataclass
class Enum:
    name: str
    values: dict[str, int]          # label -> number, in definition order

    def label(self, value: int) -> str | None:
        for k, v in self.values.items():
            if v == value:
                return k
        return None


@dataclass
class Field:
    """One value inside a message. Arrays (`count` > 1) repeat the type back to back."""

    name: str
    type: ValueType
    start_bit: int                  # bit 0 = least significant bit of byte 0 (little-endian)
    count: int = 1
    scale: float = 1.0
    offset: float = 0.0
    unit: str = ""
    min: float | None = None        # physical units
    max: float | None = None
    enum: Enum | None = None
    const: int | None = None        # fixed raw value (checked on decode, set on encode)
    description: str = ""

    @property
    def bits(self) -> int:
        return self.type.bits * self.count

    @property
    def is_scaled(self) -> bool:
        return self.scale != 1.0 or self.offset != 0.0

    def element_start(self, i: int) -> int:
        return self.start_bit + i * self.type.bits


@dataclass
class Rate:
    default: float                  # Hz; 0 means off until someone turns it on
    min: float
    max: float


@dataclass
class Message:
    name: str
    port: int
    priority: int
    sender: str                     # SENDERS
    kind: str                       # KINDS
    fields: list[Field]
    length: int                     # bytes on the wire
    rate: Rate | None = None        # streams only
    watchdog_ms: int = 0            # commands: outputs stop if none arrives for this long
    apply: str = "now"              # commands: "now" or "sync" (held until the next SYNC)
    description: str = ""

    def field(self, name: str) -> Field:
        for f in self.fields:
            if f.name == name:
                return f
        raise KeyError(f"{self.name} has no field {name!r}")


@dataclass
class Param:
    index: int
    name: str
    type: ValueType
    default: float
    min: float | None = None
    max: float | None = None
    unit: str = ""
    access: str = "rw"              # "rw" or "ro"
    persist: bool = True            # saved by SERVICE SAVE
    disarmed_only: bool = False     # SET refused while armed
    reboot_required: bool = False   # takes effect after a reset
    group: str = ""                 # for UIs: "pid", "limits", ...
    enum: Enum | None = None
    description: str = ""


@dataclass
class Action:
    index: int
    name: str
    arg: ValueType | None = None    # one argument, or none
    arg_min: float | None = None
    arg_max: float | None = None
    arg_unit: str = ""
    disarmed_only: bool = False
    description: str = ""


@dataclass
class Event:
    code: int
    name: str
    severity: str                   # SEVERITIES
    description: str = ""
    cause: str = ""


@dataclass
class IdField:
    name: str                       # priority, dst, port, src
    bits: int
    shift: int


@dataclass
class IdLayout:
    fields: list[IdField]

    def get(self, name: str) -> IdField:
        for f in self.fields:
            if f.name == name:
                return f
        raise KeyError(name)

    def make(self, **values: int) -> int:
        can_id = 0
        for f in self.fields:
            v = values.get(f.name, 0)
            if not 0 <= v < (1 << f.bits):
                raise ValueError(f"{f.name}={v} does not fit in {f.bits} bits")
            can_id |= v << f.shift
        return can_id

    def parse(self, can_id: int) -> dict[str, int]:
        return {f.name: (can_id >> f.shift) & ((1 << f.bits) - 1) for f in self.fields}


@dataclass
class NodeType:
    """A kind of node (or the protocol itself, type_id 0). Instances are just node IDs."""

    name: str
    type_id: int
    version: int
    description: str
    enums: dict[str, Enum] = field(default_factory=dict)
    messages: list[Message] = field(default_factory=list)
    params: list[Param] = field(default_factory=list)
    actions: list[Action] = field(default_factory=list)
    events: list[Event] = field(default_factory=list)
    source: str = ""                # file it came from, for error messages
    wire_hash: int = 0              # CRC-32 of everything that affects the wire; set by loader

    @property
    def hash16(self) -> int:
        return self.wire_hash & 0xFFFF

    def message(self, key: str | int) -> Message:
        for m in self.messages:
            if m.name == key or m.port == key:
                return m
        raise KeyError(f"{self.name} has no message {key!r}")

    def param(self, key: str | int) -> Param:
        for p in self.params:
            if p.name == key or p.index == key:
                return p
        raise KeyError(f"{self.name} has no parameter {key!r}")

    def action(self, key: str | int) -> Action:
        for a in self.actions:
            if a.name == key or a.index == key:
                return a
        raise KeyError(f"{self.name} has no action {key!r}")

    def event(self, key: str | int) -> Event:
        for e in self.events:
            if e.name == key or e.code == key:
                return e
        raise KeyError(f"{self.name} has no event {key!r}")


@dataclass
class NodeIds:
    broadcast: int
    unassigned: int
    first: int
    last: int


@dataclass
class Schema:
    """The protocol plus every node type. The one object the rest of the code works with."""

    protocol: NodeType
    id_layout: IdLayout
    node_ids: NodeIds
    protocol_last_port: int
    types: dict[str, NodeType] = field(default_factory=dict)

    @property
    def max_port(self) -> int:
        return (1 << self.id_layout.get("port").bits) - 1

    def type_by_id(self, type_id: int) -> NodeType | None:
        if type_id == self.protocol.type_id:
            return self.protocol
        for t in self.types.values():
            if t.type_id == type_id:
                return t
        return None

    def message_for(self, port: int, node_type: NodeType | None) -> Message | None:
        """The message a port means: protocol ports for everyone, the rest per node type."""
        if port <= self.protocol_last_port:
            try:
                return self.protocol.message(port)
            except KeyError:
                return None
        if node_type is None:
            return None
        try:
            return node_type.message(port)
        except KeyError:
            return None

    def all_params(self, node_type: NodeType) -> list[Param]:
        """A node's full parameter list: the common ones from the protocol, then its own."""
        return list(self.protocol.params) + list(node_type.params)

    def all_events(self, node_type: NodeType | None = None) -> list[Event]:
        return list(self.protocol.events) + (list(node_type.events) if node_type else [])

    def event(self, code: int, node_type: NodeType | None = None) -> Event | None:
        for e in self.all_events(node_type):
            if e.code == code:
                return e
        return None
