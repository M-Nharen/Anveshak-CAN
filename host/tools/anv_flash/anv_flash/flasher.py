"""Flashing logic behind `anv-flash`, usable as a library (e.g. by the dashboard).

Safety rules (docs/architecture.md, "Safety rules for flashing"):
  - refuse unless this host is the master and every node is DISARMED (or already in its
    bootloader, which never drives outputs);
  - the image's node type must match the node (checked here, and again by the bootloader);
  - after BOOT, wait for the node's heartbeat and check it reports the image's build ID.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass

import can
import cantools

from . import protocol as p
from .bus import (
    NMT_ENTER_BOOTLOADER,
    NODE_UNCONFIGURED,
    ROLE_MASTER,
    STATE_BOOTLOADER,
    STATE_DISARMED,
    BusView,
    NodeStatus,
    error_name,
)
from .client import BootloaderClient, BootloaderTimeout
from .image import FirmwareImage

STATE_NAMES = {0: "BOOT", 1: "DISARMED", 2: "ARMED", 3: "FAULT", 4: "ESTOP", 5: "BOOTLOADER"}
NODE_TYPE_NAMES = {1: "drive", 2: "arm", 3: "science", 4: "fpga", 5: "power"}
SAFE_STATES = (STATE_DISARMED, STATE_BOOTLOADER)


class FlashError(RuntimeError):
    """Flashing failed; the node is left in its bootloader (never with a half-valid image)."""


class SafetyRefusal(FlashError):
    """Refused before touching any node."""


@dataclass
class FlashConfig:
    settle_s: float = 0.5           # listen this long before judging the bus (NODE_HB is 10 Hz)
    heartbeat_max_age_s: float = 1.0
    enter_timeout_s: float = 3.0    # ENTER_BOOTLOADER -> first BOOTLOADER heartbeat
    silent_node_wait_s: float = 30.0  # node with no heartbeat: catch its 300 ms boot window
    boot_confirm_timeout_s: float = 5.0
    rescue_timeout_s: float = 60.0
    block_size: int = p.MAX_WRITE


@dataclass
class NodeListing:
    status: NodeStatus
    info: p.Info | None  # filled for nodes in their bootloader


class Flasher:
    def __init__(
        self,
        bus: can.BusABC,
        db: cantools.database.can.Database,
        host_id: int | None,
        progress: Callable[[str], None] | None = None,
        config: FlashConfig | None = None,
    ) -> None:
        self.bus = bus
        self.host_id = host_id
        self.view = BusView(db)
        self.cfg = config or FlashConfig()
        self._progress = progress or (lambda _msg: None)
        self.notifier = can.Notifier(bus, [self.view], timeout=0.05)
        self._settled = False

    def close(self) -> None:
        self.notifier.stop()

    def __enter__(self) -> Flasher:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # ---- observing ---------------------------------------------------------------------

    def settle(self) -> None:
        if not self._settled:
            time.sleep(self.cfg.settle_s)
            self._settled = True

    def nodes(self) -> list[NodeStatus]:
        self.settle()
        return self.view.recent_nodes(self.cfg.heartbeat_max_age_s)

    def list_nodes(self) -> list[NodeListing]:
        out = []
        for n in self.nodes():
            info = None
            if n.state == STATE_BOOTLOADER and n.node_id != NODE_UNCONFIGURED:
                with BootloaderClient(self.bus, self.notifier, n.node_id) as cl:
                    try:
                        info = cl.info(timeout=0.5)
                    except (BootloaderTimeout, p.ProtocolError):
                        info = None
            out.append(NodeListing(n, info))
        return out

    def check_safe(self) -> int:
        """Raise SafetyRefusal unless this host leads and every node is disarmed. Returns the
        master term (needed for NMT)."""
        self.settle()
        if self.host_id is None:
            raise SafetyRefusal("this host's identity is unknown: pass --host orin|rpi|tool (or set ANV_HOST)")
        me = self.view.host(self.host_id, self.cfg.heartbeat_max_age_s)
        if me is None:
            raise SafetyRefusal(f"no heartbeat from this host (node {self.host_id}): is its gateway running?")
        if me.role != ROLE_MASTER:
            raise SafetyRefusal(f"this host (node {self.host_id}) is not the master: take the lead first")
        unsafe = [n for n in self.nodes() if n.state not in SAFE_STATES]
        if unsafe:
            names = ", ".join(f"node {n.node_id} {STATE_NAMES.get(n.state, n.state)}" for n in unsafe)
            raise SafetyRefusal(f"every node must be DISARMED before flashing: {names}")
        return me.term

    # ---- flashing ----------------------------------------------------------------------

    def flash_node(self, node_id: int, image: FirmwareImage) -> None:
        if node_id == NODE_UNCONFIGURED:
            raise SafetyRefusal("node 31 may be several boards: use `anv-flash rescue --uid` for it")
        term = self.check_safe()
        status = self.view.nodes.get(node_id)
        if status is not None and status.node_type != image.header.node_type:
            raise SafetyRefusal(
                f"node {node_id} is a {NODE_TYPE_NAMES.get(status.node_type, status.node_type)} node; "
                f"this image is for {NODE_TYPE_NAMES.get(image.header.node_type, image.header.node_type)}"
            )

        with BootloaderClient(self.bus, self.notifier, node_id) as cl:
            if status is None:
                self._progress(
                    f"node {node_id}: no heartbeat; power-cycle it now, waiting up to "
                    f"{self.cfg.silent_node_wait_s:.0f} s for its bootloader"
                )
                info = self._catch_boot_window(cl, self.cfg.silent_node_wait_s)
            else:
                if status.state != STATE_BOOTLOADER:
                    self._enter_bootloader(node_id, term)
                info = self._catch_boot_window(cl, 2.0)
            self._program(cl, info, image)
        self._confirm_boot(node_id, image)

    def flash_type(self, node_type: int, image: FirmwareImage) -> list[int]:
        if image.header.node_type != node_type:
            raise SafetyRefusal(
                f"image is for node type {image.header.node_type}, not {node_type}: refusing to flash it"
            )
        self.check_safe()
        targets = [n.node_id for n in self.nodes() if n.node_type == node_type]
        if not targets:
            raise FlashError(f"no {NODE_TYPE_NAMES.get(node_type, node_type)} nodes on the bus")
        for node_id in targets:  # one at a time: the others keep running
            self._progress(f"--- node {node_id} ---")
            self.flash_node(node_id, image)
        return targets

    def rescue(self, uid32: int, image: FirmwareImage) -> int:
        """Flash the board with this UID, whatever its node ID, through the node-31 rescue
        channel. Returns the node ID it reported."""
        self.check_safe()
        with BootloaderClient(self.bus, self.notifier, NODE_UNCONFIGURED) as cl:
            self._progress(
                f"looking for board 0x{uid32:08X}: if it isn't in its bootloader, power-cycle it now "
                f"(waiting up to {self.cfg.rescue_timeout_s:.0f} s)"
            )
            deadline = time.monotonic() + self.cfg.rescue_timeout_s
            found = None
            while found is None and time.monotonic() < deadline:
                found = cl.select(uid32, timeout=0.1)
            if found is None:
                raise FlashError(f"board 0x{uid32:08X} never answered on the rescue channel")
            node_id = found[0]
            info = self._catch_boot_window(cl, 2.0)
            if info.uid32 != uid32:
                raise FlashError("a different board answered after SELECT")
            if info.node_type != image.header.node_type:
                raise SafetyRefusal(
                    f"board 0x{uid32:08X} is node type {info.node_type}; image is for {image.header.node_type}"
                )
            self._progress(f"found board 0x{uid32:08X} as node {node_id}")
            self._program(cl, info, image)
        self._confirm_boot(node_id, image)
        return node_id

    # ---- steps ------------------------------------------------------------------------

    def _enter_bootloader(self, node_id: int, term: int) -> None:
        self._progress(f"node {node_id}: ENTER_BOOTLOADER")
        assert self.host_id is not None
        self.bus.send(self.view.nmt_frame(node_id, NMT_ENTER_BOOTLOADER, self.host_id, term))
        since = time.monotonic()

        def in_bootloader(v: BusView) -> bool:
            n = v.nodes.get(node_id)
            return n is not None and n.last_seen > since and n.state == STATE_BOOTLOADER

        if not self.view.wait_for(in_bootloader, self.cfg.enter_timeout_s):
            raise FlashError(f"node {node_id} did not enter its bootloader")

    def _catch_boot_window(self, cl: BootloaderClient, wait_s: float) -> p.Info:
        deadline = time.monotonic() + wait_s
        while time.monotonic() < deadline:
            try:
                return cl.info(timeout=0.1)
            except BootloaderTimeout:
                continue
        raise FlashError(f"node {cl.channel_node}: no bootloader answered")

    def _check(self, resp: p.Response, what: str, cl: BootloaderClient, abort: bool = True) -> None:
        if resp.ok:
            return
        msg = f"{what} failed: {resp.status_name}, {error_name(resp.error_code)}"
        if abort:
            try:
                cl.abort()  # erase the partial image: the node stays in its bootloader
                msg += "; partial image erased, node left in its bootloader"
            except BootloaderTimeout:
                msg += "; ABORT got no answer, node left in its bootloader"
        raise FlashError(msg)

    def _program(self, cl: BootloaderClient, info: p.Info, image: FirmwareImage) -> None:
        try:
            self._program_steps(cl, info, image)
        except BootloaderTimeout as e:
            raise FlashError(f"{e}; the node stays in its bootloader, run the command again") from e

    def _program_steps(self, cl: BootloaderClient, info: p.Info, image: FirmwareImage) -> None:
        if info.node_type != image.header.node_type:
            raise SafetyRefusal(
                f"node {info.node_id} reports node type {info.node_type}; image is for {image.header.node_type}"
            )
        data = image.data
        if len(data) > info.region_size:
            raise FlashError(f"image ({len(data)} B) does not fit the application region ({info.region_size} B)")
        node = info.node_id

        # Erase one unit per request: each answers within the 5 s ERASE timeout even for an
        # F4 128 KB sector, where one request for the whole image might not.
        units = [(off, size) for off, size in info.erase_units() if off < len(data)]
        for i, (off, size) in enumerate(units, 1):
            self._progress(f"node {node}: erase {i}/{len(units)}")
            self._check(cl.erase(off, size), "ERASE", cl, abort=False)

        block = min(self.cfg.block_size, info.max_write)
        started = time.monotonic()
        for off in range(0, len(data), block):
            self._check(cl.write(off, data[off : off + block]), f"WRITE at 0x{off:X}", cl)
            done = min(off + block, len(data))
            self._progress(f"node {node}: wrote {done}/{len(data)} B")
        elapsed = max(time.monotonic() - started, 1e-3)
        self._progress(f"node {node}: {len(data) / 1024 / elapsed:.1f} KB/s")

        self._check(cl.verify(image.header.image_crc32), "VERIFY", cl)
        self._check(cl.commit(), "COMMIT", cl)
        self._progress(f"node {node}: committed, booting")
        self._check(cl.boot(), "BOOT", cl, abort=False)

    def _confirm_boot(self, node_id: int, image: FirmwareImage) -> None:
        since = time.monotonic()
        want = image.header.build_id16

        def running(v: BusView) -> bool:
            n = v.nodes.get(node_id)
            return n is not None and n.last_seen > since and n.state != STATE_BOOTLOADER

        if not self.view.wait_for(running, self.cfg.boot_confirm_timeout_s):
            raise FlashError(f"node {node_id}: no application heartbeat after BOOT")
        got = self.view.nodes[node_id].build_id16
        if got != want:
            raise FlashError(f"node {node_id} runs build 0x{got:04X}, expected 0x{want:04X}")
        self._progress(f"node {node_id}: running build 0x{got:04X}, OK")
