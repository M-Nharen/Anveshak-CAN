"""What the flasher knows about the bus: node heartbeats and host heartbeats, decoded with the
generated layouts in defs/rover.dbc (never hand-packed)."""

from __future__ import annotations

import sys
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import can
import cantools

REPO_ROOT = Path(__file__).resolve().parents[4]
DEFAULT_DBC = REPO_ROOT / "defs" / "rover.dbc"

CLASS_HEARTBEAT = 6
CLASS_BOOTLOADER = 7
NODE_ORIN = 1
NODE_RPI = 2
NODE_TOOL = 3
NODE_UNCONFIGURED = 31
HOST_NAMES = {NODE_ORIN: "orin", NODE_RPI: "rpi", NODE_TOOL: "tool"}
HOST_IDS = {name: node for node, name in HOST_NAMES.items()}

STATE_BOOT = 0
STATE_DISARMED = 1
STATE_ARMED = 2
STATE_FAULT = 3
STATE_ESTOP = 4
STATE_BOOTLOADER = 5
ROLE_MASTER = 2

NMT_ENTER_BOOTLOADER = 4

# Every node's NODE_HB has drive1's layout (only the CAN ID differs), so one message
# definition decodes them all -- including node 31, which has no entry of its own in the DBC.
_NODE_HB_TEMPLATE = "DRIVE1_NODE_HB"


def can_id(cls: int, node: int, sub: int) -> int:
    """11-bit ID = (class << 8) | (node << 3) | sub."""
    return ((cls & 0x7) << 8) | ((node & 0x1F) << 3) | (sub & 0x7)


def load_db(path: str | Path | None = None) -> cantools.database.can.Database:
    return cantools.database.load_file(str(path or DEFAULT_DBC))


def error_name(code: int) -> str:
    """errors.yaml name for `code` (generated anv_can_defs), or the hex code."""
    if code == 0:
        return "none"
    try:
        from anv_can_defs.errors import ERRORS
    except ImportError:
        generated = REPO_ROOT / "defs" / "generated" / "python"
        if generated.is_dir() and str(generated) not in sys.path:
            sys.path.append(str(generated))
        try:
            from anv_can_defs.errors import ERRORS
        except ImportError:
            return f"0x{code:04X}"
    info = ERRORS.get(code)
    return f"{info.name} (0x{code:04X})" if info else f"0x{code:04X}"


@dataclass
class NodeStatus:
    node_id: int
    state: int
    node_type: int
    build_id16: int
    uptime_s: int
    last_seen: float


@dataclass
class HostStatus:
    node_id: int
    role: int
    term: int
    last_seen: float


class BusView(can.Listener):
    """Latest heartbeat of every node and host, updated from a can.Notifier thread."""

    def __init__(self, db: cantools.database.can.Database) -> None:
        self.db = db
        self._node_hb = db.get_message_by_name(_NODE_HB_TEMPLATE)
        self._host_hb = {
            db.get_message_by_name(f"HOST_HB_{name.upper()}").frame_id: node for node, name in HOST_NAMES.items()
        }
        self.nodes: dict[int, NodeStatus] = {}
        self.hosts: dict[int, HostStatus] = {}
        self._cond = threading.Condition()

    def on_message_received(self, msg: can.Message) -> None:
        if msg.is_extended_id or msg.is_error_frame or msg.is_remote_frame:
            return
        fid = msg.arbitration_id
        now = time.monotonic()
        try:
            if fid in self._host_hb:
                d = self.db.get_message_by_frame_id(fid).decode(bytes(msg.data), decode_choices=False)
                status = HostStatus(self._host_hb[fid], int(d["ROLE"]), int(d["TERM"]), now)
                with self._cond:
                    self.hosts[status.node_id] = status
                    self._cond.notify_all()
            elif (fid >> 8) == CLASS_HEARTBEAT and (fid & 0x7) == 0:
                d = self._node_hb.decode(bytes(msg.data), decode_choices=False)
                node = (fid >> 3) & 0x1F
                status = NodeStatus(
                    node, int(d["STATE"]), int(d["NODE_TYPE"]), int(d["FW_BUILD_ID"]), int(d["UPTIME_S"]), now
                )
                with self._cond:
                    self.nodes[node] = status
                    self._cond.notify_all()
        except (ValueError, KeyError, cantools.database.errors.DecodeError):
            return

    def wait_for(self, predicate: Callable[[BusView], bool], timeout: float) -> bool:
        """Block until `predicate(self)` holds (checked on every heartbeat) or `timeout` passes."""
        deadline = time.monotonic() + timeout
        with self._cond:
            while not predicate(self):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._cond.wait(remaining)
            return True

    def recent_nodes(self, max_age: float) -> list[NodeStatus]:
        cutoff = time.monotonic() - max_age
        with self._cond:
            return sorted((n for n in self.nodes.values() if n.last_seen >= cutoff), key=lambda n: n.node_id)

    def host(self, node_id: int, max_age: float) -> HostStatus | None:
        with self._cond:
            h = self.hosts.get(node_id)
        if h is None or h.last_seen < time.monotonic() - max_age:
            return None
        return h

    def nmt_frame(self, target: int, command: int, master_id: int, term: int) -> can.Message:
        nmt = self.db.get_message_by_name("NMT")
        data = nmt.encode(
            {"TARGET_NODE": target, "COMMAND": command, "MASTER_NODE_ID": master_id, "MASTER_TERM": term & 0xFF}
        )
        return can.Message(arbitration_id=nmt.frame_id, data=data, is_extended_id=False)
