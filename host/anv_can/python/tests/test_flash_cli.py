"""anv-flash against a fake node that speaks the OTA and log wire protocol directly (no real
anv_node): BEGIN/DATA/END/ABORT on transfer_service `ota`, one REQUEST/DATA round for `log`.
Byte layout must match firmware/lib/anv_node/src/anv_node_ota.h and anv_node_diag.h exactly;
this is the host side of that same protocol, not a port of the firmware.
"""
from __future__ import annotations

import io
import os
import shutil
import struct
import subprocess
import tempfile
import threading
import zlib

import pytest
from conftest import FakeNode, eventually

import anv_can
from anv_can import flash_cli

SERVICE_OTA, SERVICE_LOG = 1, 2
BEGIN, DATA, END, ABORT, ACK = 0, 1, 2, 3, 4
LOG_REQUEST, LOG_DATA = 0, 1


@pytest.fixture(scope="module")
def schema():
    return anv_can.load_schema()


def run(bus, *argv: str) -> tuple[int, str]:
    out = io.StringIO()
    code = flash_cli.main(["--wait", "1", *argv], lambda _name: bus.connect(), out)
    return code, out.getvalue()


def _write_tmp(data: bytes) -> str:
    f = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
    f.write(data)
    f.close()
    return f.name


class FakeOtaNode:
    """The transfer-port half of a node: OTA and the log dump, for anv-flash's own tests. Pair
    it with conftest.FakeNode (on the same bus/link) for discovery (heartbeat, settings)."""

    def __init__(self, bus: anv_can.TestBus | anv_can.Link, schema: anv_can.Schema, node_id: int,
                type_id: int, chip: int = 1, peer: int = flash_cli.TOOL_ID_DEFAULT):
        self.id = node_id
        self.type_id = type_id
        self.chip = chip
        self.armed = False
        self.fail_at: int | None = None  # simulate a flash write failure at/after this offset
        self.image = b""
        self.commits = 0
        self.aborts = 0
        self.log_entries: list[tuple[int, int, int, int, int]] = []  # code,detail,uptime,sev,seq
        self._buf = b""
        self._len = 0
        self._crc = 0
        link = bus.connect() if isinstance(bus, anv_can.TestBus) else bus
        self.ch = anv_can.IsoTpChannel(link, schema, node_id, peer)
        self._run = True
        self._th = threading.Thread(target=self._loop, daemon=True)
        self._th.start()

    def stop(self) -> None:
        self._run = False
        self._th.join()

    def _loop(self) -> None:
        while self._run:
            try:
                payload = self.ch.recv(timeout_ms=200)
            except anv_can.IsoTpError:
                continue
            except EOFError:
                return
            if not payload:
                continue
            service, body = payload[0], payload[1:]
            if service == SERVICE_OTA:
                self._ota(body)
            elif service == SERVICE_LOG:
                self._log(body)

    def _ack(self, status: int, next_offset: int = 0) -> None:
        self.ch.send(bytes([SERVICE_OTA, ACK, status]) + struct.pack("<I", next_offset))

    def _ota(self, body: bytes) -> None:
        if not body:
            return
        msg = body[0]
        if msg == BEGIN:
            if self.armed:
                self._ack(3)  # NOT_DISARMED
                return
            type_id, chip, image_len, crc = struct.unpack_from("<HBII", body, 1)
            if type_id != self.type_id or chip != self.chip:
                self._ack(1)  # BAD_TYPE
                return
            self._buf, self._len, self._crc = b"", image_len, crc
            self._ack(0)
        elif msg == DATA:
            offset = struct.unpack_from("<I", body, 1)[0]
            chunk = body[5:]
            if offset != len(self._buf):
                self._ack(4, len(self._buf))  # BAD_OFFSET
                return
            if self.fail_at is not None and offset >= self.fail_at:
                self._ack(5, len(self._buf))  # WRITE_FAILED
                return
            self._buf += chunk
            self._ack(0, len(self._buf))
        elif msg == END:
            if len(self._buf) != self._len or (zlib.crc32(self._buf) & 0xFFFFFFFF) != self._crc:
                self.aborts += 1
                self._ack(6)  # CRC_MISMATCH
                return
            self.image = bytes(self._buf)
            self.commits += 1
            self._ack(0)
        elif msg == ABORT:
            self.aborts += 1
            self._buf = b""

    def _log(self, body: bytes) -> None:
        if not body or body[0] != LOG_REQUEST:
            return
        out = bytearray([SERVICE_LOG, LOG_DATA])
        for code, detail, uptime, sev, seq in self.log_entries:
            out += struct.pack("<HHHBB", code, detail, uptime, sev, seq)
        self.ch.send(bytes(out))


def test_list(schema):
    bus = anv_can.TestBus()
    n = FakeNode(bus, schema, "esp32_bench", 20)
    code, out = run(bus, "list", "x")
    assert code == 0
    assert any(line.startswith(" 20") and "esp32_bench" in line and "idle" in line
              for line in out.splitlines())
    n.stop()


def test_flash_one_node(schema):
    bus = anv_can.TestBus()
    type_id = schema.type("esp32_bench").type_id
    n = FakeNode(bus, schema, "esp32_bench", 20)
    o = FakeOtaNode(bus, schema, 20, type_id)
    image = bytes(range(256)) * 20  # 5120 bytes: several DATA chunks at the chosen size
    tmp = _write_tmp(image)
    try:
        code, out = run(bus, "flash", "x", "20", tmp, "--chunk", "1024")
        assert code == 0, out
        assert "100%" in out and "1/1 node(s) flashed" in out
        assert o.image == image and o.commits == 1
    finally:
        n.stop()
        o.stop()


def test_flash_rejects_wrong_type(schema):
    bus = anv_can.TestBus()
    n = FakeNode(bus, schema, "esp32_bench", 20)
    o = FakeOtaNode(bus, schema, 20, schema.type("esp32_bench").type_id + 1)  # expects another type
    tmp = _write_tmp(b"abc")
    try:
        code, out = run(bus, "flash", "x", "20", tmp)
        assert code == 1 and "wrong type or chip" in out
        assert o.commits == 0
    finally:
        n.stop()
        o.stop()


def test_flash_refuses_armed_node(schema):
    bus = anv_can.TestBus()
    n = FakeNode(bus, schema, "esp32_bench", 20)
    n.armed = True  # what the host discovers (the node's heartbeat); the CLI checks this first
    o = FakeOtaNode(bus, schema, 20, schema.type("esp32_bench").type_id)
    o.armed = True  # the firmware-side backstop, in case the discovered state is stale
    tmp = _write_tmp(b"abc")
    try:
        code, out = run(bus, "flash", "x", "20", tmp)
        # Either the CLI's own pre-check or the node's BEGIN refusal catches it; both are the
        # system working, and which one wins is a timing detail (how stale the discovered
        # state happens to be).
        assert code == 1 and ("armed, skipping" in out or "node is armed" in out)
        assert o.commits == 0
    finally:
        n.stop()
        o.stop()


def test_flash_detects_corruption(schema):
    bus = anv_can.TestBus()
    type_id = schema.type("esp32_bench").type_id
    n = FakeNode(bus, schema, "esp32_bench", 20)
    o = FakeOtaNode(bus, schema, 20, type_id)
    o.fail_at = 10  # the node's flash write fails partway through
    tmp = _write_tmp(bytes(range(256)) * 4)
    try:
        code, out = run(bus, "flash", "x", "20", tmp, "--chunk", "32")
        assert code == 1 and "flash write failed" in out
        assert o.commits == 0
    finally:
        n.stop()
        o.stop()


def test_flash_type_target_and_force(schema):
    bus = anv_can.TestBus()
    type_id = schema.type("esp32_bench").type_id
    n1 = FakeNode(bus, schema, "esp32_bench", 20)
    n2 = FakeNode(bus, schema, "esp32_bench", 21)
    o1 = FakeOtaNode(bus, schema, 20, type_id)
    o2 = FakeOtaNode(bus, schema, 21, type_id)
    tmp = _write_tmp(b"xy" * 50)
    try:
        code, out = run(bus, "flash", "x", "type:esp32_bench", tmp, "--force")
        assert code == 0 and "2/2 node(s) flashed" in out
        assert o1.commits == 1 and o2.commits == 1
    finally:
        for x in (n1, n2, o1, o2):
            x.stop()


def test_log(schema):
    bus = anv_can.TestBus()
    n = FakeNode(bus, schema, "esp32_bench", 20)
    o = FakeOtaNode(bus, schema, 20, schema.type("esp32_bench").type_id)
    o.log_entries = [(0x0101, 7, 3, 1, 1), (0x0102, 0, 4, 2, 2)]
    try:
        code, out = run(bus, "log", "x", "20")
        assert code == 0, out
        lines = out.splitlines()
        assert "can_error_passive" in lines[0] and "detail=7" in lines[0] and "warning" in lines[0]
        assert "can_bus_off" in lines[1] and "error" in lines[1]
    finally:
        n.stop()
        o.stop()


def _vcan_up() -> bool:
    return os.path.exists("/sys/class/net/vcan0")


@pytest.mark.skipif(not _vcan_up() or shutil.which("anv-flash") is None, reason="needs vcan0")
def test_vcan_flash(schema):
    """The installed anv-flash on a real SocketCAN interface, against a fake node on vcan0."""
    link = anv_can.open_socketcan("vcan0")
    n = FakeNode(link, schema, "esp32_bench", 20)
    o = FakeOtaNode(link, schema, 20, schema.type("esp32_bench").type_id)
    tmp = _write_tmp(bytes(range(256)) * 10)
    try:
        def cli(*args: str) -> str:
            r = subprocess.run(["anv-flash", *args], capture_output=True, text=True, timeout=20)
            assert r.returncode == 0, r.stderr
            return r.stdout

        assert "esp32_bench" in cli("list", "vcan0")
        out = cli("flash", "vcan0", "20", tmp, "--chunk", "1024")
        assert "100%" in out
        assert eventually(lambda: o.commits == 1)
    finally:
        n.stop()
        o.stop()
