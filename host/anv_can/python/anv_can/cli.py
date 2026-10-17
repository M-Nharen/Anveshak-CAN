"""anv-can: watch and poke the bus using the schema.

    anv-can dump vcan0                       # decode everything, live
    anv-can dump can0 --type 20=motor_controller --count 100
    anv-can send vcan0 protocol nmt command=arm term=3 --src 1 --dst 20
    anv-can send vcan0 motor_controller setpoint mode=velocity "target=[1.5,-2]" --src 1 --dst 20
    anv-can send vcan0 04504001#02960038FF0000   # a raw frame, cansend format

dump learns which node is which type from heartbeats, so node-type messages decode as soon
as a node's first heartbeat goes by (or straight away with --type).
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from collections.abc import Callable
from typing import Any, TextIO

from . import (
    CodecError,
    Frame,
    Link,
    NodeType,
    Schema,
    decode_frame,
    encode,
    load_schema,
    make_id,
)


def _fmt(v: Any) -> str:
    if isinstance(v, float):
        return f"{v:.6g}"
    if isinstance(v, list):
        return "[" + ",".join(_fmt(x) for x in v) + "]"
    return str(v)


class Decoder:
    """Decodes frames, remembering each node's type from its heartbeat."""

    def __init__(self, schema: Schema, types: dict[int, NodeType] | None = None) -> None:
        self.schema = schema
        self.types: dict[int, NodeType] = dict(types or {})
        hb = schema.protocol.message("heartbeat")
        self._hb_port = hb.port

    def line(self, frame: Frame, t0_ns: int = 0) -> str:
        stamp = f"{(frame.rx_ns - t0_ns) / 1e9:10.4f}" if frame.rx_ns and t0_ns else " " * 10
        if not frame.extended:
            return f"{stamp}  {frame}  (11-bit frame, not v2)"
        p = self.schema.parse_id(frame.id)
        # A node's own messages decode with its type; a command to a node with the receiver's.
        nt = self.types.get(p["src"]) or self.types.get(p["dst"])
        d = decode_frame(self.schema, frame.id, frame.data, nt)
        if d.message is not None and d.port == self._hb_port and d.values and not d.error:
            known = self.schema.type_by_id(int(d.values["type_id"]))
            if known is not None:
                self.types[d.src] = known
        head = (f"{stamp}  {frame.id:08X}  {d.src:3d} -> {d.dst:<3d} p{d.priority}  "
                f"[{len(frame.data)}] {frame.data.hex(' ').upper():<23}")
        if d.message is None:
            return f"{head}  port {d.port}: unknown{'' if nt else ' (sender type not known yet)'}"
        owner = "protocol" if d.port <= self.schema.protocol_last_port else (nt.name if nt else "?")
        name = f"{owner}.{d.message.name}"
        if d.error:
            return f"{head}  {name}  ERROR {d.error}"
        if d.values is None:
            return f"{head}  {name}"
        fields = " ".join(f"{k}={_fmt(v)}" for k, v in d.values.items() if k != "reserved")
        return f"{head}  {name}  {fields}"


def dump(link: Link, schema: Schema, out: TextIO, *, count: int = 0, duration_s: float = 0,
         types: dict[int, NodeType] | None = None) -> int:
    dec = Decoder(schema, types)
    seen, t0 = 0, 0
    deadline = time.monotonic() + duration_s if duration_s else None
    while count == 0 or seen < count:
        if deadline is not None and time.monotonic() >= deadline:
            break
        try:
            frame = link.recv(100)
        except EOFError:
            break
        if frame is None:
            continue
        t0 = t0 or frame.rx_ns
        print(dec.line(frame, t0), file=out, flush=True)
        seen += 1
    return seen


def parse_value(text: str) -> Any:
    """field=value values: numbers, true/false, [lists] or enum labels."""
    low = text.lower()
    if low in ("true", "false"):
        return low == "true"
    try:
        v = json.loads(text)
        if isinstance(v, (int, float, list)):
            return v
    except ValueError:
        pass
    return text


def build_frame(schema: Schema, args: list[str], src: int, dst: int, raw: bool) -> Frame:
    if len(args) == 1 and "#" in args[0]:
        return Frame.parse(args[0])
    if len(args) < 2:
        raise ValueError("expected ID#DATA, or TYPE MESSAGE [field=value ...]")
    nt = schema.type(args[0])
    msg = nt.message(args[1])
    values: dict[str, Any] = {}
    for item in args[2:]:
        name, sep, value = item.partition("=")
        if not sep:
            raise ValueError(f"expected field=value, got {item!r}")
        values[name] = parse_value(value)
    return Frame(make_id(schema, msg, src=src, dst=dst), encode(msg, values, raw=raw))


def _type_overrides(schema: Schema, items: list[str]) -> dict[int, NodeType]:
    out = {}
    for item in items:
        node, _, name = item.partition("=")
        out[int(node, 0)] = schema.type(name)
    return out


def main(argv: list[str] | None = None,
         open_link: Callable[[str], Link] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="anv-can", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--schema", help="schema.json (default: $ANV_SCHEMA_JSON or the bundled copy)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump", help="decode live traffic")
    d.add_argument("interface")
    d.add_argument("--type", action="append", default=[], metavar="NODE=TYPE",
                   help="decode node NODE as TYPE without waiting for its heartbeat")
    d.add_argument("--count", type=int, default=0, help="stop after this many frames")
    d.add_argument("--duration", type=float, default=0, help="stop after this many seconds")
    s = sub.add_parser("send", help="send one frame")
    s.add_argument("interface")
    s.add_argument("frame", nargs="+", help="ID#DATA, or TYPE MESSAGE [field=value ...]")
    s.add_argument("--src", type=lambda x: int(x, 0), default=1, help="sender node ID (default 1)")
    s.add_argument("--dst", type=lambda x: int(x, 0), default=0, help="receiver (default 0 = all)")
    s.add_argument("--raw", action="store_true", help="values are wire integers")
    a = ap.parse_args(argv)

    try:
        schema = load_schema(a.schema)
    except (FileNotFoundError, ValueError) as e:
        print(f"anv-can: {e}", file=sys.stderr)
        return 2
    opener = open_link or _open_socketcan
    try:
        if a.cmd == "dump":
            types = _type_overrides(schema, a.type)
            link = opener(a.interface)
            try:
                dump(link, schema, sys.stdout, count=a.count, duration_s=a.duration, types=types)
            except KeyboardInterrupt:
                pass
            return 0
        frame = build_frame(schema, a.frame, a.src, a.dst, a.raw)
        link = opener(a.interface)
        if not link.send(frame):
            print("anv-can: send failed (interface down or queue full)", file=sys.stderr)
            return 1
        print(frame)
        return 0
    except (KeyError, ValueError, CodecError) as e:
        print(f"anv-can: {e}", file=sys.stderr)
        return 2


def _open_socketcan(interface: str) -> Link:
    from . import open_socketcan
    return open_socketcan(interface)


if __name__ == "__main__":
    sys.exit(main())
