"""BenchGateway: the v2 host that stays up on the bench RPi.

It is `anv_can.Host` (host heartbeat, ID allocator, discovery, leader election) plus the few things
the touch screen needs on top: an event log, the latest stream values per node, bench text
(protocol `text`, port 14) with echoes, and flashing one node over CAN. `state()` reports all of it
in the same JSON shape the old 11-bit gateway's bridge used, so the dashboard keeps working.

Three links on the same interface, as anv-flash does: Host's thread reads continuously for
discovery, which would race the text reader and an ISO-TP transfer for the same frames otherwise.
"""

from __future__ import annotations

import queue
import struct
import threading
import time
import zlib
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import anv_can
from anv_can.flash_cli import CHIP_NAMES, DEFAULT_CHUNK, FlashError, Session

# The old gateway's node_state numbers, which the dashboard's health colours use.
LEGACY_STATE = {"boot": 0, "unassigned": 0, "idle": 1, "armed": 2, "fault": 3, "estop": 4, "updating": 5}
LEGACY_ROLE = {"booting": 0, "standby": 1, "leader": 2, "observer": 3}  # same numbers as host_role
HW_NAMES = {1: "ESP32", 2: "ESP32-S3", 3: "ESP32-C3"}
IMAGE_CHIP_IDS = {0: 1, 9: 2, 5: 3}  # esp_image_header_t chip_id -> protocol info_page chip
EVENTS_KEPT = 50
TEXTS_KEPT = 40
TEXT_LEN = 7


def now_ns() -> int:
    return time.monotonic_ns()


def mac_str(mac: int) -> str:
    return ":".join(f"{(mac >> s) & 0xFF:02x}" for s in range(40, -1, -8))


@dataclass
class GatewayConfig:
    host_id: int = 2  # the RPi
    tie_break: int = 0  # 0 = host_id
    id_first: int = 16
    id_last: int = 200
    id_table: str = ""
    auto_takeover: bool = False


@dataclass
class FlashJob:
    node_id: int
    file: str
    phase: str = "starting"
    pct: float = 0.0
    msg: str = ""
    state: str = "running"  # running, done, failed
    started_ns: int = field(default_factory=now_ns)
    cancel: threading.Event = field(default_factory=threading.Event)


class BenchGateway:
    def __init__(
        self, open_link: Callable[[], anv_can.Link], schema: anv_can.Schema, cfg: GatewayConfig
    ) -> None:
        self.schema = schema
        self.cfg = cfg
        self._host_link = open_link()
        self._text_link = open_link()
        self._transfer_link = open_link()
        self.host = anv_can.Host(
            self._host_link,
            schema,
            host_id=cfg.host_id,
            tie_break=cfg.tie_break,
            allocator="auto",
            id_first=cfg.id_first,
            id_last=cfg.id_last,
            id_table=cfg.id_table,
            auto_takeover=cfg.auto_takeover,
        )
        self._text_msg = schema.protocol.message("text")
        self._lock = threading.Lock()  # guards everything below
        self._events: deque[dict[str, Any]] = deque(maxlen=EVENTS_KEPT)
        self._texts: deque[dict[str, Any]] = deque(maxlen=TEXTS_KEPT)
        self._values: dict[int, dict[str, dict[str, Any]]] = {}  # node -> stream -> values
        self._chips: dict[int, int] = {}
        self._rx_count: dict[int, int] = {}
        self._rx_total = 0
        self._fps = (now_ns(), 0, 0.0)  # window start, frames at start, last rate
        self._leader_lost = False
        self._text_seq = 0
        self._flash: FlashJob | None = None
        self._chip_queue: queue.Queue[int] = queue.Queue()
        self._running = False
        self._threads: list[threading.Thread] = []

    # --- lifecycle -------------------------------------------------------------------------
    def start(self) -> None:
        self.host.set_callbacks(
            node_appeared=self._on_appeared,
            node_lost=self._on_lost,
            node_rebooted=self._on_rebooted,
            telemetry=self._on_telemetry,
            event=self._on_node_event,
            id_assigned=self._on_id_assigned,
            estop=self._on_estop,
            role_changed=self._on_role,
            leader_lost=self._on_leader_lost,
            leader_changed=self._on_leader_changed,
        )
        self.host.start()
        self._running = True
        for target in (self._text_loop, self._chip_loop):
            t = threading.Thread(target=target, daemon=True)
            t.start()
            self._threads.append(t)

    def stop(self) -> None:
        self._running = False
        job = self._flash
        if job is not None:
            job.cancel.set()
        self._chip_queue.put(0)
        self.host.stop()
        for link in (self._host_link, self._text_link, self._transfer_link):
            link.close()
        for t in self._threads:
            t.join(timeout=2)

    # --- Host callbacks (Host's thread: never call blocking Host methods from here) -----------
    def _event(self, kind: str, node: int = 0, **extra: Any) -> None:
        with self._lock:
            self._events.append({"type": kind, "time_ns": now_ns(), "node_id": node, **extra})

    def _on_appeared(self, info: anv_can.NodeInfo, returned: bool) -> None:
        self._event("NODE_ALIVE", info.id)
        self._chip_queue.put(info.id)  # ask for its chip on the worker thread

    def _on_lost(self, info: anv_can.NodeInfo) -> None:
        self._event("NODE_MISSING", info.id)

    def _on_rebooted(self, info: anv_can.NodeInfo) -> None:
        self._event("NODE_REBOOTED", info.id)

    def _on_telemetry(self, node: int, message: str, values: dict[str, Any], ts: int) -> None:
        with self._lock:
            self._values.setdefault(node, {})[message] = values

    def _on_node_event(self, e: dict[str, Any]) -> None:
        self._event(
            "NODE_EVENT",
            e["node"],
            event=e.get("name") or f"0x{e['code']:04x}",
            severity=e.get("severity", ""),
            cleared=bool(e.get("cleared")),
        )

    def _on_id_assigned(self, node: int, mac: int, type_id: int) -> None:
        self._event("NODE_ID_ASSIGNED", node, mac=mac_str(mac))

    def _on_estop(self, src: int, reason: int) -> None:
        self._event("ESTOP_SEEN", src)

    def _on_role(self, role: str, term: int) -> None:
        self._event("ROLE_CHANGED", self.cfg.host_id, role=LEGACY_ROLE.get(role, 0), term=term)

    def _on_leader_lost(self, leader: int) -> None:
        self._leader_lost = True
        self._event("LEADER_LOST", leader)

    def _on_leader_changed(self, leader: int, term: int) -> None:
        if leader:
            self._leader_lost = False

    # --- workers ----------------------------------------------------------------------------
    def _chip_loop(self) -> None:
        while self._running:
            node = self._chip_queue.get()
            if not self._running or not node:
                continue
            try:
                chip = int(self.host.info(node, "chip"))
            except anv_can.HostError:
                continue
            with self._lock:
                self._chips[node] = chip

    def _text_loop(self) -> None:
        """Counts every frame (activity dots, frames per second) and collects text for this host."""
        unassigned = self.schema.node_ids["unassigned"]
        while self._running:
            try:
                f = self._text_link.recv(50)
            except EOFError:
                return
            if f is None:
                continue
            idp = self.schema.parse_id(f.id)
            src, dst, port = idp["src"], idp["dst"], idp["port"]
            with self._lock:
                self._rx_total += 1
                if src != unassigned:
                    self._rx_count[src] = self._rx_count.get(src, 0) + 1
            if port != self._text_msg.port or dst not in (0, self.cfg.host_id):
                continue
            try:
                v = anv_can.decode(self._text_msg, f.data, raw=True, labels=False)
            except anv_can.CodecError:
                continue
            text = "".join(chr(c) if 0x20 <= c < 0x7F else "?" for c in v["chars"] if c)
            with self._lock:
                self._texts.append(
                    {"time_ns": now_ns(), "dir": "rx", "node_id": src, "seq": v["seq"], "text": text}
                )

    # --- commands (from the bridge) ------------------------------------------------------------
    def send_text(self, node: int, text: str) -> tuple[bool, str, int]:
        if not text or len(text) > TEXT_LEN or any(not 0x20 <= ord(c) < 0x7F for c in text):
            return False, f"text must be 1-{TEXT_LEN} printable ASCII characters", 0
        if node and not (self.schema.node_ids["first"] <= node <= self.schema.node_ids["last"]):
            return False, "node must be 0 (everyone) or a node ID", 0
        with self._lock:
            self._text_seq = self._text_seq % 255 + 1
            seq = self._text_seq
        chars = [ord(c) for c in text] + [0] * (TEXT_LEN - len(text))
        frame = anv_can.Frame(
            anv_can.make_id(self.schema, self._text_msg, src=self.cfg.host_id, dst=node),
            anv_can.encode(self._text_msg, {"seq": seq, "chars": chars}, raw=True),
        )
        try:
            self._text_link.send(frame)
        except Exception as e:  # noqa: BLE001 - the link reports a refused send as a plain error
            return False, f"send failed: {e}", 0
        with self._lock:
            self._texts.append({"time_ns": now_ns(), "dir": "tx", "node_id": node, "seq": seq, "text": text})
        return True, "ok", seq

    def forget_node(self, node: int) -> tuple[bool, str]:
        try:
            self.host.forget_id(node)
        except anv_can.HostError as e:
            return False, str(e)
        # Make the board ask again too; only the leader may send NMT, so this is best effort.
        try:
            self.host.nmt(node, "forget_id")
        except anv_can.HostError:
            pass
        return True, "ok"

    def flash(self, node: int, path: str) -> tuple[bool, str]:
        with self._lock:
            if self._flash is not None and self._flash.state == "running":
                return False, f"node {self._flash.node_id} is being flashed; wait or cancel it"
            job = FlashJob(node_id=node, file=Path(path).name)
            self._flash = job
        threading.Thread(target=self._flash_worker, args=(job, path), daemon=True).start()
        return True, "ok"

    def flash_cancel(self) -> bool:
        job = self._flash
        if job is None or job.state != "running":
            return False
        job.cancel.set()
        return True

    def _flash_worker(self, job: FlashJob, path: str) -> None:
        def fail(msg: str) -> None:
            job.phase, job.state, job.msg = "error", "failed", msg

        try:
            image = Path(path).read_bytes()
        except OSError as e:
            return fail(f"can't read the image: {e}")
        info = self.host.node(job.node_id)
        if info is None or not info.online:
            return fail(f"node {job.node_id} is not on the bus")
        if anv_can.enum_label(self.schema, "node_state", info.state) == "armed":
            return fail(f"node {job.node_id} is armed; disarm it first")
        try:
            chip = int(self.host.info(job.node_id, "chip"))
        except anv_can.HostError as e:
            return fail(f"node {job.node_id} didn't report its chip: {e}")
        error = image_problem(image, chip)
        if error:
            return fail(error)

        job.phase, job.msg = (
            "write",
            f"{len(image)} bytes for an {HW_NAMES.get(chip, CHIP_NAMES.get(chip, '?'))}",
        )
        crc = zlib.crc32(image) & 0xFFFFFFFF
        # 10 s per reply: boards on firmware that erases the whole slot at BEGIN take seconds to ACK it.
        s = Session(self._transfer_link, self.schema, self.cfg.host_id, job.node_id, timeout_ms=10000)
        try:
            s.begin(info.type_id, chip, len(image), crc)
            offset = 0
            while offset < len(image):
                if job.cancel.is_set():
                    s.abort()
                    return fail("cancelled; the node keeps its current image")
                offset = s.data(offset, image[offset : offset + DEFAULT_CHUNK])
                job.pct = 100.0 * offset / len(image)
            job.phase = "verify"
            s.end()
        except (FlashError, anv_can.IsoTpError) as e:
            with_abort(s)
            return fail(str(e))

        job.phase, job.pct, job.msg = "confirm", 100.0, "sent; waiting for the node to restart"
        if self._wait_for_restart(job.node_id, timeout_s=25.0):
            job.phase, job.state, job.msg = "done", "done", f"node {job.node_id} restarted with the new image"
        else:
            fail(f"node {job.node_id} did not come back within 25 s")

    def _wait_for_restart(self, node: int, timeout_s: float) -> bool:
        """The node reboots into the new image: wait until it is heard again with a fresh uptime."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline and self._running:
            info = self.host.node(node)
            if (
                info is not None
                and info.online
                and info.uptime_s < 30
                and anv_can.enum_label(self.schema, "node_state", info.state) != "updating"
            ):
                return True
            time.sleep(0.2)
        return False

    # --- state for the dashboard -----------------------------------------------------------------
    def state(self) -> dict[str, Any]:
        now = now_ns()
        h = self.host
        nodes = h.nodes()
        role = LEGACY_ROLE.get(h.role, 0)
        with self._lock:
            start, frames_at, fps = self._fps
            if now - start >= 1_000_000_000:
                fps = (self._rx_total - frames_at) * 1e9 / (now - start)
                self._fps = (now, self._rx_total, fps)
            node_dicts = [self._node_json(n, now) for n in nodes]
            events = [
                dict(e, age_ms=(now - e["time_ns"]) / 1e6, name=node_name(e["node_id"])) for e in self._events
            ]
            texts = [
                dict(
                    t,
                    age_ms=(now - t["time_ns"]) / 1e6,
                    name="ALL" if t["node_id"] == 0 else node_name(t["node_id"]),
                )
                for t in self._texts
            ]
            job = self._flash
            flash = (
                None
                if job is None
                else {
                    "node_id": job.node_id,
                    "file": job.file,
                    "phase": job.phase,
                    "pct": round(job.pct, 1),
                    "msg": job.msg,
                    "state": job.state,
                    "elapsed_s": round((now - job.started_ns) / 1e9),
                }
            )
            rx_total = self._rx_total
        peers = {p.id: p for p in h.hosts()}
        return {
            "time_ns": now,
            "host_id": self.cfg.host_id,
            "role": role,
            "role_name": h.role.upper(),
            "term": h.term,
            "believed_master": h.leader,
            "leader_lost": self._leader_lost and not h.leader,
            "armed": False,
            "estop_latched": h.estop_latched,
            "cycles_sent": 0,
            "late_cycles": 0,
            "worst_lateness_us": 0,
            "bus": {
                "rx_frames": rx_total,
                "tx_frames": 0,
                "tx_failures": 0,
                "error_frames": 0,
                "undecoded_frames": 0,
                "bus_off": False,
                "rx_fps": round(fps, 1),
            },
            "peers": [
                {
                    "host": i,
                    "seen": i in peers or i == self.cfg.host_id,
                    "role": LEGACY_ROLE.get(peers[i].role, 0) if i in peers else role,
                    "term": peers[i].term if i in peers else h.term,
                }
                for i in (1, 2, 3)
            ],
            "nodes": node_dicts,
            "online_count": sum(1 for n in node_dicts if n["alive"]),
            "id_allocator": True,
            "id_table": [
                {"mac": mac_str(mac), "node_id": nid, "type_id": tid} for mac, nid, tid in h.id_table()
            ],
            "events": events,
            "texts": texts,
            "flash": flash,
        }

    def _node_json(self, n: anv_can.NodeInfo, now: int) -> dict[str, Any]:
        state_label = anv_can.enum_label(self.schema, "node_state", n.state) or str(n.state)
        chip = self._chips.get(n.id, 0)
        values = []
        t = n.type
        if t is not None and n.usable:
            for m in t.messages:
                if m.kind != "stream" or m.name not in self._values.get(n.id, {}):
                    continue
                latest = self._values[n.id][m.name]
                for f in m.fields:
                    v = latest.get(f.name)
                    if isinstance(v, (int, float)):
                        values.append({"name": f.name, "value": round(float(v), 2), "unit": f.unit or ""})
        return {
            "node_id": n.id,
            "name": node_name(n.id),
            "seen": True,
            "alive": n.online,
            "age_ms": None,
            "state": LEGACY_STATE.get(state_label, 0),
            "state_name": state_label.upper(),
            "has_hb": True,
            "node_type": n.type_id,
            "type_name": t.name if t is not None else f"type {n.type_id}",
            "compat": n.compat,
            "fw_build_id": 0,
            "uptime_s": n.uptime_s,
            "reboot_count": n.reboots,
            "tec": 0,
            "rec": 0,
            "can_error_state": n.can_state,
            "bootloader_running": state_label == "updating",
            "debug_level": n.debug_level,
            "rx_count": self._rx_count.get(n.id, 0),
            "mac": mac_str(n.mac) if n.mac else None,
            "hw_target": chip,
            "hw_name": HW_NAMES.get(chip, ""),
            "values": values,
        }


def node_name(node: int) -> str:
    return f"NODE{node}"


def image_problem(image: bytes, node_chip: int) -> str | None:
    """Refuse what a node would refuse anyway, with a clearer message: not an app image, or the
    other chip's image. Arduino's '.ino.bin' starts with the 0xE9 image header (chip id at 12)."""
    if len(image) < 0x30 or image[0] != 0xE9:
        hint = " (use the '.ino.bin', not the '.ino.merged.bin')" if len(image) >= 0x10000 else ""
        return "not an ESP32 app image" + hint
    chip = IMAGE_CHIP_IDS.get(struct.unpack_from("<H", image, 12)[0])
    if chip is None:
        return "image is for an unsupported chip"
    if node_chip and chip != node_chip:
        return f"image is for an {HW_NAMES.get(chip, '?')}, but the node is an {HW_NAMES.get(node_chip, '?')}"
    return None


def with_abort(s: Session) -> None:
    try:
        s.abort()
    except (FlashError, anv_can.IsoTpError, OSError):
        pass  # best effort: the node gives up on a silent session by itself
