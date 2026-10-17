"""anv-flash command line.

    anv-flash info  --node 16
    anv-flash flash --node 16 build/esp32.esp32.esp32/bench_esp32_arduino.ino.bin
    anv-flash flash --node 18 --json firmware.bin      # one JSON progress object per line (dashboard)
"""

from __future__ import annotations

import argparse
import json
import signal
import socket
import sys
import time
from pathlib import Path
from typing import Any

import can
import cantools

from .image import ImageError, load_image
from .protocol import FlashError, Updater

DBC_PATH = Path(__file__).resolve().parents[3] / "defs" / "rover.dbc"
NODE_STATE_ARMED = 2
CONFIRM_TIMEOUT_S = 25.0


class Reporter:
    def __init__(self, as_json: bool) -> None:
        self.as_json = as_json
        self._last_pct = -1

    def emit(self, phase: str, pct: float = 0.0, msg: str = "", **extra: Any) -> None:
        if self.as_json:
            print(json.dumps({"phase": phase, "pct": round(pct * 100, 1), "msg": msg, **extra}), flush=True)
        elif phase == "write":
            p = int(pct * 100)
            if p != self._last_pct:
                self._last_pct = p
                print(f"\r  writing {p:3d}%", end="", flush=True)
        else:
            if self._last_pct >= 0:
                print()
                self._last_pct = -1
            print(f"{phase}: {msg}" if msg else phase, flush=True)


def gateway_state(path: str) -> dict[str, Any]:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2.0)
        s.connect(path)
        s.sendall(b'{"id":1,"cmd":"state"}\n')
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    reply = json.loads(buf)
    if not reply.get("ok"):
        raise FlashError(f"gateway: {reply.get('error')}")
    return reply["state"]


def check_allowed(state: dict[str, Any], node_id: int) -> None:
    """Bench rule: this host flashes when it leads, or when no host leads (the same host that hands
    out node IDs). The node must be on the bus and not armed."""
    if state["role"] != 2 and not state.get("id_allocator"):
        raise FlashError("another host leads the bus; flash from the leader (or make this host lead)")
    node = next((n for n in state["nodes"] if n["node_id"] == node_id), None)
    if node is None or not node["alive"]:
        raise FlashError(f"node {node_id} is not on the bus")
    if node["state"] == NODE_STATE_ARMED:
        raise FlashError(f"node {node_id} is armed; disarm it first")


def wait_for_new_image(bus: can.BusABC, node_id: int, build_id: int, old_build: int | None) -> str:
    """After BOOT: wait for the node's heartbeat and check it runs the image we sent."""
    hb = cantools.database.load_file(DBC_PATH).get_message_by_name("DRIVE1_NODE_HB")  # shared layout
    hb_id = 0x600 | (node_id << 3)
    deadline = time.monotonic() + CONFIRM_TIMEOUT_S
    seen_build = None
    while time.monotonic() < deadline:
        msg = bus.recv(timeout=0.5)
        if msg is None or msg.arbitration_id != hb_id:
            continue
        d = hb.decode(msg.data, decode_choices=False, scaling=False)
        seen_build = d["FW_BUILD_ID"]
        if seen_build == build_id:
            return f"node {node_id} is running the new image (build {build_id:04x})"
    if seen_build is None:
        raise FlashError(f"node {node_id} did not come back within {CONFIRM_TIMEOUT_S:.0f} s")
    raise FlashError(
        f"node {node_id} came back with build {seen_build:04x}, not {build_id:04x}"
        + (" (it rolled back to the old image)" if seen_build == old_build else "")
    )


def node_build(state: dict[str, Any] | None, node_id: int) -> int | None:
    if state is None:
        return None
    node = next((n for n in state["nodes"] if n["node_id"] == node_id), None)
    return node["fw_build_id"] if node and node.get("has_hb") else None


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="anv-flash", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--interface", default="socketcan", help="python-can interface (default socketcan)")
    p.add_argument("--channel", default="can1", help="CAN channel (default can1)")
    p.add_argument("--bridge", default="/run/anveshak/gateway.sock", help="gateway bridge socket, for the safety checks")
    p.add_argument("--no-gateway-check", action="store_true", help="skip the leader / armed checks (no gateway running)")
    sub = p.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("info", help="ask a node about its update slot")
    i.add_argument("--node", type=int, required=True)
    f = sub.add_parser("flash", help="flash an app image (.ino.bin) to one node")
    f.add_argument("--node", type=int, required=True)
    f.add_argument("--json", action="store_true", help="progress as JSON lines")
    f.add_argument("file")
    return p.parse_args(argv)


def run(args: argparse.Namespace, bus: can.BusABC, report: Reporter) -> None:
    if not 4 <= args.node <= 30:
        raise FlashError("node must be 4-30")
    updater = Updater(bus, args.node)
    if args.cmd == "info":
        info = updater.info()
        report.emit("info", 1.0, f"node {args.node}: {info.target_name}, running slot ota_{info.running_slot}, "
                    f"update slot {info.slot_bytes // 1024} KB, protocol v{info.protocol_version}")
        return

    image = load_image(args.file)
    state = None
    if not args.no_gateway_check:
        try:
            state = gateway_state(args.bridge)
        except OSError as exc:
            raise FlashError(f"can't reach the gateway at {args.bridge} ({exc}); use --no-gateway-check to skip") from exc
        check_allowed(state, args.node)
    report.emit("check", 0.0, f"{Path(args.file).name}: {image.size} bytes for an {image.target_name}, build {image.build_id:04x}")
    started = time.monotonic()
    updater.flash(image, lambda phase, pct: report.emit(phase, pct))
    report.emit("confirm", 1.0, f"sent in {time.monotonic() - started:.0f} s; waiting for node {args.node} to restart")
    report.emit("done", 1.0, wait_for_new_image(bus, args.node, image.build_id, node_build(state, args.node)))


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    report = Reporter(getattr(args, "json", False))

    def on_term(*_: object) -> None:
        raise KeyboardInterrupt  # unwinds through Updater.flash, which tells the node to abort

    signal.signal(signal.SIGTERM, on_term)
    try:
        bus = can.Bus(interface=args.interface, channel=args.channel)
    except (OSError, can.CanError) as exc:
        report.emit("error", 0.0, f"can't open {args.channel}: {exc}")
        return 1
    try:
        run(args, bus, report)
        return 0
    except (FlashError, ImageError, OSError) as exc:
        report.emit("error", 0.0, str(exc))
        return 1
    except KeyboardInterrupt:
        report.emit("error", 0.0, "cancelled; the node keeps its current image")
        return 130
    finally:
        bus.shutdown()
