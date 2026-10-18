"""Test doubles: the v2 fake nodes from host/anv_can/python/tests (discovery, OTA) plus a text-echo
responder like the bench sketch's. Loaded by path so this folder needs no conftest.py of its own."""

from __future__ import annotations

import importlib.util
import struct
import sys
import threading
import time
from pathlib import Path

import anv_can

ANV_CAN_TESTS = Path(__file__).resolve().parents[3] / "anv_can" / "python" / "tests"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


# anv_can's flash tests do `from conftest import ...`: make that resolve to their conftest.
_conftest = sys.modules.get("conftest")
if _conftest is None or not hasattr(_conftest, "FakeNode"):
    _conftest = _load("conftest", ANV_CAN_TESTS / "conftest.py")
FakeNode = _conftest.FakeNode
eventually = _conftest.eventually
FakeOtaNode = _load("anv_can_flash_tests", ANV_CAN_TESTS / "test_flash_cli.py").FakeOtaNode


class TextEcho:
    """Answers protocol `text` sent to `node` (or to everyone) with the same text and seq, back to
    the sender, as the bench sketch does; `say()` sends the node's own text (seq 0)."""

    def __init__(self, bus: anv_can.TestBus, schema: anv_can.Schema, node: int) -> None:
        self.s, self.node = schema, node
        self.msg = schema.protocol.message("text")
        self.link = bus.connect()
        self.received: list[str] = []
        self._run = True
        self._th = threading.Thread(target=self._loop, daemon=True)
        self._th.start()

    def stop(self) -> None:
        self._run = False
        self._th.join()

    def _send(self, dst: int, seq: int, chars: list[int]) -> None:
        self.link.send(
            anv_can.Frame(
                anv_can.make_id(self.s, self.msg, src=self.node, dst=dst),
                anv_can.encode(self.msg, {"seq": seq, "chars": chars}, raw=True),
            )
        )

    def say(self, text: str) -> None:
        self._send(0, 0, [ord(c) for c in text] + [0] * (7 - len(text)))

    def _loop(self) -> None:
        while self._run:
            try:
                f = self.link.recv(20)
            except EOFError:
                return
            if f is None:
                continue
            idp = self.s.parse_id(f.id)
            if idp["port"] != self.msg.port or idp["dst"] not in (0, self.node) or idp["src"] == self.node:
                continue
            v = anv_can.decode(self.msg, f.data, raw=True, labels=False)
            self.received.append("".join(chr(c) for c in v["chars"] if c))
            self._send(idp["src"], v["seq"], list(v["chars"]))


def app_image(chip_id: int = 0, size: int = 6000) -> bytes:
    """Enough of an Arduino '.ino.bin' for the gateway's checks: 0xE9 magic, chip id at 12."""
    b = bytearray((i * 13) & 0xFF for i in range(size))
    b[0] = 0xE9
    struct.pack_into("<H", b, 12, chip_id)
    return bytes(b)


def wait_for(cond, timeout: float = 3.0) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.02)
    return cond()
