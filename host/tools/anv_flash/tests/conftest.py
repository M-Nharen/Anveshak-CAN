"""Fixtures: a private python-can virtual channel per test, a host heartbeat, fake nodes and
stamped images."""

from __future__ import annotations

import itertools
import os
import threading
import time
import zlib
from collections.abc import Iterator

import can
import pytest

from anv_flash.bus import NODE_TOOL, ROLE_MASTER, load_db
from anv_flash.flasher import FlashConfig, Flasher
from anv_flash.image import HEADER_LEN, HEADER_SLOT, ImageHeader, stamp
from fake_node import FakeNode

_counter = itertools.count()

FAST = FlashConfig(
    settle_s=0.3,
    heartbeat_max_age_s=0.5,
    enter_timeout_s=2.0,
    silent_node_wait_s=10.0,
    boot_confirm_timeout_s=3.0,
    rescue_timeout_s=10.0,
)


def make_image(node_type: int = 1, build_id: int = 0x1234ABCD, size: int = 9000, seed: int = 1) -> bytes:
    """A stamped image, built the way the firmware build does (C header placeholder + stamp)."""
    body = bytes((i * 131 + seed * 7 + (i >> 8)) & 0xFF for i in range(size))
    placeholder = ImageHeader(0x48564E41, 1, node_type, 0xFFFFFFFF, 0xFFFFFFFF, 0x00010203, 0xFFFFFFFF, "", 0)
    raw = placeholder.pack()[:HEADER_LEN] + b"\xff" * (HEADER_SLOT - HEADER_LEN) + body
    return stamp(raw, build_id, f"g{build_id:08x}")


class HostHeartbeat:
    """HOST_HB from one host at 20 Hz, as its gateway would send it."""

    def __init__(self, channel: str, db, host_id: int = NODE_TOOL, role: int = ROLE_MASTER, term: int = 7) -> None:
        self.role = role
        self.term = term
        self._msg = db.get_message_by_name({1: "HOST_HB_ORIN", 2: "HOST_HB_RPI", 3: "HOST_HB_TOOL"}[host_id])
        self._bus = can.Bus(interface="virtual", channel=channel, receive_own_messages=False)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            data = self._msg.encode({"ROLE": self.role, "TIEBREAK": 3, "TERM": self.term, "BELIEVED_MASTER": 3,
                                     "FLAGS": 0, "UPTIME_S": 0})
            self._bus.send(can.Message(arbitration_id=self._msg.frame_id, data=data, is_extended_id=False))
            self._stop.wait(0.05)

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self._bus.shutdown()


class Rig:
    def __init__(self) -> None:
        self.channel = f"anvflash-{os.getpid()}-{next(_counter)}"
        self.db = load_db()
        self.host = HostHeartbeat(self.channel, self.db)
        self.nodes: list[FakeNode] = []
        self.tool_bus = can.Bus(interface="virtual", channel=self.channel, receive_own_messages=False)
        self._flashers: list[Flasher] = []
        self.log: list[str] = []

    def node(self, node_id: int, image: bytes | None = None, **kw) -> FakeNode:
        uid = kw.pop("uid", zlib.crc32(bytes([node_id])).to_bytes(4, "little") * 3)
        n = FakeNode(self.channel, self.db, node_id, uid, image=image, **kw)
        self.nodes.append(n)
        if kw.get("crash_build") is None:  # running nodes are up before the test starts
            deadline = time.monotonic() + 2.0
            while n.mode == "listen" and time.monotonic() < deadline:
                time.sleep(0.01)
        return n

    def flasher(self, host_id: int | None = NODE_TOOL) -> Flasher:
        for old in self._flashers:  # one Notifier per bus at a time
            old.close()
        self._flashers.clear()
        f = Flasher(self.tool_bus, self.db, host_id, progress=self.log.append, config=FlashConfig(**vars(FAST)))
        self._flashers.append(f)
        return f

    def close(self) -> None:
        for f in self._flashers:
            f.close()
        for n in self.nodes:
            n.stop()
        self.host.stop()
        self.tool_bus.shutdown()


@pytest.fixture
def rig() -> Iterator[Rig]:
    r = Rig()
    yield r
    r.close()
