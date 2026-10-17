"""anv-flash: flash firmware to a node (or every node of a type, or a list) over CAN, and read
a node's black-box log. Works from any Linux host (Raspberry Pi, Jetson, a laptop) against the
v2 node library (anv_node_ota / anv_diag) -- this is not the old 11-bit anv_flash under
host/tools/, which stays for the current STM32/ESP32 apps.

    anv-flash list vcan0
    anv-flash flash vcan0 20 build/esp32.esp32.esp32/bench_esp32_arduino.ino.bin
    anv-flash flash vcan0 type:esp32_bench firmware.bin   # every node of that type, one by one
    anv-flash flash vcan0 20,21,22 firmware.bin           # a list
    anv-flash flash vcan0 20 firmware.bin --force         # clear a stuck session first (rescue)
    anv-flash log vcan0 20

Wire protocol: schema/protocol.yaml's `transfer` port, transfer_service `ota` (BEGIN/DATA/END/
ABORT, each ACKed) and `log` (one REQUEST, one DATA reply). See firmware/lib/anv_node's
anv_node_ota.h and anv_node_diag.h for the exact byte layout; this module is the host side of
that same protocol, built directly on anv_can.IsoTpChannel (no firmware code is shared, but the
byte offsets must match).
"""
from __future__ import annotations

import argparse
import contextlib
import os
import struct
import sys
import time
import zlib
from pathlib import Path
from typing import TextIO

from . import Host, HostError, IsoTpChannel, IsoTpError, Link, Schema, enum_label, load_schema

SERVICE_OTA = 1
SERVICE_LOG = 2

OTA_BEGIN, OTA_DATA, OTA_END, OTA_ABORT, OTA_ACK = 0, 1, 2, 3, 4
OTA_STATUS = {
    0: "ok", 1: "wrong type or chip", 2: "image too big", 3: "node is armed",
    4: "offset out of sync", 5: "flash write failed", 6: "CRC mismatch",
    7: "no session (BEGIN first)", 8: "busy with another host", 9: "node has no OTA backend",
}
LOG_REQUEST, LOG_DATA = 0, 1

CHIP_NAMES = {0: "unknown", 1: "esp32", 2: "esp32s3", 3: "esp32c3"}
CHIP_IDS = {v: k for k, v in CHIP_NAMES.items() if k != 0}

DEFAULT_CHUNK = 2048  # anv_node_ota.h's ANV_NODE_OTA_MAX_CHUNK
TOOL_ID_DEFAULT = int(os.environ.get("ANV_HOST_ID", "250"), 0)


class FlashError(Exception):
    pass


def _open(open_link, name: str) -> Link:
    if open_link is not None:
        return open_link(name)
    from . import open_socketcan
    return open_socketcan(name)


def _ota_ack(data: bytes) -> tuple[int, int]:
    if len(data) < 7 or data[0] != SERVICE_OTA or data[1] != OTA_ACK:
        raise FlashError(f"not an OTA ack: {data!r}")
    status = data[2]
    next_offset = struct.unpack_from("<I", data, 3)[0]
    return status, next_offset


class Session:
    """One flashing session with one node, over its own ISO-TP channel."""

    def __init__(self, link: Link, schema: Schema, tool_id: int, node: int, timeout_ms: int = 2000):
        self.node = node
        self.ch = IsoTpChannel(link, schema, tool_id, node, timeout_ms=timeout_ms)

    def _exchange(self, body: bytes, expect_ack: bool = True) -> tuple[int, int]:
        try:
            self.ch.send(body)
        except IsoTpError as e:
            raise FlashError(f"node {self.node}: send failed ({e})") from e
        if not expect_ack:
            return 0, 0
        try:
            reply = self.ch.recv()
        except IsoTpError as e:
            raise FlashError(f"node {self.node}: no reply ({e})") from e
        return _ota_ack(reply)

    def abort(self) -> None:
        with contextlib.suppress(FlashError):
            self._exchange(bytes([SERVICE_OTA, OTA_ABORT]), expect_ack=False)

    def begin(self, type_id: int, chip: int, image_len: int, crc: int) -> None:
        body = bytes([SERVICE_OTA, OTA_BEGIN]) + struct.pack("<HBII", type_id, chip, image_len, crc)
        status, _ = self._exchange(body)
        if status != 0:
            raise FlashError(f"node {self.node}: BEGIN refused ({OTA_STATUS.get(status, status)})")

    def data(self, offset: int, chunk: bytes) -> int:
        body = bytes([SERVICE_OTA, OTA_DATA]) + struct.pack("<I", offset) + chunk
        status, next_offset = self._exchange(body)
        if status != 0:
            raise FlashError(f"node {self.node}: DATA refused at offset {offset} "
                             f"({OTA_STATUS.get(status, status)})")
        return next_offset

    def end(self) -> None:
        status, _ = self._exchange(bytes([SERVICE_OTA, OTA_END]))
        if status != 0:
            raise FlashError(f"node {self.node}: the node rejected the finished image "
                             f"({OTA_STATUS.get(status, status)})")

    def log(self) -> list[dict[str, int]]:
        self.ch.send(bytes([SERVICE_LOG, LOG_REQUEST]))
        reply = self.ch.recv()
        if len(reply) < 2 or reply[0] != SERVICE_LOG or reply[1] != LOG_DATA:
            raise FlashError(f"node {self.node}: bad log reply {reply!r}")
        body = reply[2:]
        out = []
        for i in range(0, len(body) - 7, 8):
            code, detail, uptime_s, flags, seq = struct.unpack_from("<HHHBB", body, i)
            out.append({"code": code, "detail": detail, "uptime_s": uptime_s, "seq": seq,
                        "severity": flags & 0x03, "cleared": bool(flags & 0x04)})
        return out


def _resolve_targets(host: Host, text: str) -> list[int]:
    if text == "all":
        return sorted(n.id for n in host.nodes())
    if text.startswith("type:"):
        name = text[len("type:"):]
        return sorted(n.id for n in host.nodes() if n.type is not None and n.type.name == name)
    return sorted({int(x, 0) for x in text.split(",")})


def _severity_name(schema: Schema, value: int) -> str:
    return enum_label(schema, "severity", value)


def _event_name(host: Host, schema: Schema, node: int, code: int) -> str:
    info = host.node(node)
    if info is not None and info.type is not None:
        e = info.type.event(code)
        if e is not None:
            return e.name
    e = schema.protocol.event(code)
    return e.name if e is not None else f"0x{code:04x}"


def cmd_list(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    time.sleep(a.wait)
    print(f"{'ID':>3}  {'TYPE':<20} {'STATE':<10} {'BUILD':<10}  CHIP", file=out)
    for n in sorted(host.nodes(), key=lambda n: n.id):
        tname = n.type.name if n.type is not None else f"type {n.type_id}"
        state = enum_label(schema, "node_state", n.state)
        try:
            build = f"{int(host.info(n.id, 'fw_build')):08x}"
        except HostError:
            build = "?"
        chip = CHIP_NAMES.get(_chip_of(host, n.id), "?")
        print(f"{n.id:>3}  {tname:<20} {state:<10} {build:<10}  {chip}", file=out)
    return 0


def _chip_of(host: Host, node: int) -> int:
    try:
        return int(host.info(node, "chip"))
    except HostError:
        return 0


def _flash_one(host: Host, schema: Schema, link: Link, a: argparse.Namespace, node: int,
               image: bytes, crc: int, out: TextIO) -> bool:
    if not host.wait_for_node(node, int(a.wait * 1000)):
        print(f"node {node}: not heard, skipping", file=out)
        return False
    info = host.node(node)
    if enum_label(schema, "node_state", info.state) == "armed":
        print(f"node {node}: armed, skipping (disarm it first)", file=out)
        return False
    type_id = a.type_id if a.type_id is not None else info.type_id
    chip = CHIP_IDS[a.chip] if a.chip else _chip_of(host, node)

    s = Session(link, schema, a.tool_id, node, timeout_ms=a.timeout_ms)
    if a.force:
        s.abort()
    try:
        s.begin(type_id, chip, len(image), crc)
        offset = 0
        while offset < len(image):
            chunk = image[offset:offset + a.chunk]
            offset = s.data(offset, chunk)
            pct = 100 * offset // len(image)
            print(f"\rnode {node}: {pct:3d}% ({offset}/{len(image)} bytes)", end="", file=out,
                  flush=True)
        print(file=out)
        s.end()
    except FlashError as e:
        print(f"\nnode {node}: {e}", file=out)
        return False
    print(f"node {node}: sent; it reboots and runs the new image on probation "
          "until it confirms itself", file=out)
    return True


def cmd_flash(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO, link: Link) -> int:
    image = Path(a.file).read_bytes()
    if not image:
        print("anv-flash: empty image", file=sys.stderr)
        return 2
    crc = zlib.crc32(image) & 0xFFFFFFFF
    if a.target == "all" or a.target.startswith("type:"):
        time.sleep(a.wait)  # a single node ID is already waited for in _flash_one
    targets = _resolve_targets(host, a.target)
    if not targets:
        print(f"anv-flash: no nodes match {a.target!r}", file=out)
        return 1
    print(f"flashing {len(image)} bytes (crc32 {crc:08x}) to node(s): {targets}", file=out)
    ok = 0
    for node in targets:
        if _flash_one(host, schema, link, a, node, image, crc, out):
            ok += 1
    print(f"{ok}/{len(targets)} node(s) flashed", file=out)
    return 0 if ok == len(targets) else 1


def cmd_log(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO, link: Link) -> int:
    if not host.wait_for_node(a.node, int(a.wait * 1000)):
        print(f"anv-flash: node {a.node} not heard", file=sys.stderr)
        return 1
    s = Session(link, schema, a.tool_id, a.node, timeout_ms=a.timeout_ms)
    try:
        entries = s.log()
    except FlashError as e:
        print(f"anv-flash: {e}", file=sys.stderr)
        return 1
    if not entries:
        print("(empty)", file=out)
        return 0
    for e in entries:
        name = _event_name(host, schema, a.node, e["code"])
        cleared = " (cleared)" if e["cleared"] else ""
        print(f"#{e['seq']:<3} {_severity_name(schema, e['severity']):<8} {name}{cleared}  "
              f"detail={e['detail']}  uptime={e['uptime_s']}s", file=out)
    return 0


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="anv-flash", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--schema", help="schema.json (default: $ANV_SCHEMA_JSON or the bundled copy)")
    ap.add_argument("--tool-id", type=lambda x: int(x, 0), default=TOOL_ID_DEFAULT,
                    help="this tool's node ID on the bus (default $ANV_HOST_ID or 250)")
    ap.add_argument("--wait", type=float, default=2.0,
                    help="seconds to wait for a node's heartbeat")
    ap.add_argument("--timeout-ms", type=int, default=2000, help="per-message ISO-TP timeout")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("list", help="nodes, their type, state, firmware build and chip")
    sp.add_argument("interface")
    sp.set_defaults(fn=cmd_list)

    sp = sub.add_parser("flash", help="send a firmware image to one node, a type, a list, or all")
    sp.add_argument("interface")
    sp.add_argument("target", help="node ID, 'type:<name>', 'id,id,...', or 'all'")
    sp.add_argument("file", help="the .bin image")
    sp.add_argument("--chunk", type=int, default=DEFAULT_CHUNK, help="bytes per DATA message")
    sp.add_argument("--type-id", type=int,
                    help="override the schema type_id (default: the node's own)")
    sp.add_argument("--chip", choices=sorted(CHIP_IDS),
                    help="override the chip (default: the node's own)")
    sp.add_argument("--force", action="store_true",
                    help="clear a stuck session first (rescue a node left mid-update)")
    sp.set_defaults(fn=cmd_flash)

    sp = sub.add_parser("log", help="read a node's black-box log")
    sp.add_argument("interface")
    sp.add_argument("node", type=lambda x: int(x, 0))
    sp.set_defaults(fn=cmd_log)

    return ap


def main(argv: list[str] | None = None, open_link=None, out: TextIO | None = None) -> int:
    a = _parser().parse_args(argv)
    out = out or sys.stdout
    try:
        schema = load_schema(a.schema)
    except (FileNotFoundError, ValueError) as e:
        print(f"anv-flash: {e}", file=sys.stderr)
        return 2
    try:
        # Two separate connections: Host's background thread reads continuously for discovery,
        # which would otherwise race an ISO-TP transfer's own recv() for the same frames.
        host_link = _open(open_link, a.interface)
        transfer_link = _open(open_link, a.interface)
    except ValueError as e:
        print(f"anv-flash: {e}", file=sys.stderr)
        return 2
    # Discovery only: anv-flash never leads or arms anything.
    host = Host(host_link, schema, host_id=a.tool_id, allocator="off", observer=True)
    try:
        host.start()
        if a.cmd == "list":
            return cmd_list(host, schema, a, out)
        if a.cmd == "flash":
            return cmd_flash(host, schema, a, out, transfer_link)
        return cmd_log(host, schema, a, out, transfer_link)
    except HostError as e:
        print(f"anv-flash: {e}", file=sys.stderr)
        return 1
    except (FlashError, ValueError) as e:
        print(f"anv-flash: {e}", file=sys.stderr)
        return 1
    finally:
        host.stop()


if __name__ == "__main__":
    sys.exit(main())
