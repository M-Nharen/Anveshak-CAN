"""A pretend bench ESP32 for anv-flash tests: ISO-TP receive, the update commands of ota_can.cpp,
and heartbeats that switch to the new build ID after BOOT."""

from __future__ import annotations

import struct
import threading
import time
import zlib
from pathlib import Path

import can
import cantools

DBC = cantools.database.load_file(Path(__file__).resolve().parents[4] / "defs" / "rover.dbc")
HB = DBC.get_message_by_name("DRIVE1_NODE_HB")
CHIP_IDS = {0: 0, 1: 9}


def make_image(hw_target: int = 0, size: int = 9000, build_id: int = 0xBEEF) -> bytes:
    b = bytearray((i * 7) & 0xFF for i in range(size))
    b[0] = 0xE9
    struct.pack_into("<H", b, 12, CHIP_IDS[hw_target])
    struct.pack_into("<I", b, 0x20, 0xABCD5432)
    b[0x50:0x70] = b"bench_esp32_arduino".ljust(32, b"\0")
    b[0xB0], b[0xB1] = build_id & 0xFF, build_id >> 8
    return bytes(b)


class FakeNode:
    def __init__(self, channel: str, node_id: int = 16, hw_target: int = 0, old_build: int = 0x1111, *,
                 armed: bool = False, corrupt: bool = False, lose_cf_once: bool = False, rollback: bool = False,
                 silent: bool = False) -> None:
        self.bus = can.Bus(interface="virtual", channel=channel, receive_own_messages=False)
        self.node_id, self.hw_target, self.build = node_id, hw_target, old_build
        self.old_build = old_build
        self.armed, self.corrupt, self.lose_cf_once, self.rollback, self.silent = armed, corrupt, lose_cf_once, rollback, silent
        self.flash = bytearray()
        self.session = False
        self.size = self.crc = 0
        self.committed = False
        self.aborted = False
        self.writes = 0
        self._rx = bytearray()
        self._rx_len = 0
        self._sn = 0
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._running = False
        self._thread.join()
        self.bus.shutdown()

    # --- bus helpers
    def _send(self, arb: int, data: bytes) -> None:
        self.bus.send(can.Message(arbitration_id=arb, is_extended_id=False, data=data.ljust(8, b"\xaa")))

    def _reply(self, cmd: int, status: int, extra: bytes = b"") -> None:
        body = bytes([cmd | 0x40, status]) + extra
        self._send(0x700 | (self.node_id << 3) | 1, bytes([len(body)]) + body)

    def _heartbeat(self) -> None:
        data = HB.encode({"STATE": 2 if self.armed else 1, "NODE_TYPE": 3, "PROTOCOL_VERSION": 1, "CAN_ERROR_STATE": 0,
                          "BOOTLOADER_RUNNING": 0, "UNSAVED_PARAM_CHANGES": 0, "DEBUG_LEVEL": 0,
                          "FW_BUILD_ID": self.build, "UPTIME_S": 1}, scaling=False)  # fmt: skip
        self._send(0x600 | (self.node_id << 3), data)

    def _loop(self) -> None:
        next_hb = 0.0
        boot_at = None
        while self._running:
            now = time.monotonic()
            if now >= next_hb and boot_at is None:
                self._heartbeat()
                next_hb = now + 0.1
            if boot_at is not None and now >= boot_at:
                self.build = self.old_build if self.rollback else struct.unpack_from("<H", self.flash, 0xB0)[0]
                boot_at = None
            msg = self.bus.recv(timeout=0.005)
            if msg is None or msg.arbitration_id != 0x700 | (self.node_id << 3) or self.silent:
                continue
            if self._isotp(bytes(msg.data)) == "boot":
                boot_at = time.monotonic() + 0.3

    def _isotp(self, d: bytes) -> str | None:
        pci = d[0] >> 4
        if pci == 0:
            return self._handle(d[1 : 1 + (d[0] & 0x0F)])
        if pci == 1:
            self._rx_len = ((d[0] & 0x0F) << 8) | d[1]
            self._rx = bytearray(d[2:8])
            self._sn = 1
            self._send(0x700 | (self.node_id << 3) | 1, bytes([0x30, 0, 0]))
            return None
        if pci == 2:
            if self.lose_cf_once and self._sn == 5:
                self.lose_cf_once = False
                self._sn = -1  # pretend this frame never arrived: the rest of the block is discarded
                return None
            if (d[0] & 0x0F) != self._sn:
                return None
            self._rx += d[1:8]
            self._sn = (self._sn + 1) & 0x0F
            if len(self._rx) >= self._rx_len:
                return self._handle(bytes(self._rx[: self._rx_len]))
        return None

    def _handle(self, m: bytes) -> str | None:
        cmd = m[0]
        if cmd == 1:
            self._reply(1, 0, bytes([self.hw_target, 1, 0, 0x00, 0x05]))  # 1280 KB slot
        elif cmd == 2:
            size, crc, target = struct.unpack_from("<IIB", m, 1)
            if self.armed:
                return self._reply(2, 7)
            if target != self.hw_target:
                return self._reply(2, 3)
            self.session, self.size, self.crc, self.flash = True, size, crc, bytearray()
            self._reply(2, 0)
        elif cmd == 3:
            offset = struct.unpack_from("<I", m, 1)[0]
            data = bytearray(m[5:])
            if not self.session or offset != len(self.flash):
                return self._reply(3, 2, struct.pack("<I", len(self.flash)))
            if self.corrupt and offset == 0:
                data[100] ^= 0xFF
            self.flash += data
            self.writes += 1
            self._reply(3, 0, struct.pack("<I", len(self.flash)))
        elif cmd == 4:
            ok = len(self.flash) == self.size and zlib.crc32(self.flash) & 0xFFFFFFFF == self.crc
            self._reply(4, 0 if ok else 6)
        elif cmd == 5:
            self.committed = True
            self._reply(5, 0)
        elif cmd == 6:
            self._reply(6, 0)
            return "boot"
        elif cmd == 7:
            self.aborted, self.session = True, False
            self._reply(7, 0)
        return None
