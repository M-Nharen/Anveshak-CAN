"""The update commands carried over ISO-TP on class 7 (host -> node sub 0, node -> host sub 1).

Each request starts with a command byte; the node replies with a single frame [cmd | 0x40, status, ...].
Mirrors firmware/nodes/bench_esp32_arduino/ota_can.cpp."""

from __future__ import annotations

import contextlib
import struct
from collections.abc import Callable
from dataclasses import dataclass

import can

from .image import HW_TARGET_NAMES, FirmwareImage
from .isotp import IsoTpError, IsoTpLink, IsoTpTimeout

INFO, BEGIN, WRITE, VERIFY, COMMIT, BOOT, ABORT = range(1, 8)
STATUS_NAMES = {
    0: "OK",
    1: "BAD_STATE",
    2: "BAD_ARG",
    3: "WRONG_TARGET",
    4: "TOO_BIG",
    5: "FLASH_ERROR",
    6: "CRC_MISMATCH",
    7: "ARMED",
    8: "BAD_IMAGE",
    9: "UNKNOWN_CMD",
}
STATUS_HELP = {
    3: "the image is for the other chip (WROOM vs S3)",
    4: "the image doesn't fit in the node's update slot",
    5: "the node couldn't write its flash",
    6: "the data arrived corrupted; try again",
    7: "the node is armed; disarm it first",
    8: "the node rejected the image as invalid",
}
OK, BAD_ARG = 0, 2
BLOCK = 2048
WRITE_RETRIES = 3


class FlashError(Exception):
    pass


@dataclass(frozen=True)
class NodeInfo:
    hw_target: int
    protocol_version: int
    running_slot: int
    slot_bytes: int

    @property
    def target_name(self) -> str:
        return HW_TARGET_NAMES.get(self.hw_target, f"target {self.hw_target}")


def boot_ids(node_id: int) -> tuple[int, int]:
    base = 0x700 | (node_id << 3)
    return base, base | 1


class Updater:
    def __init__(self, bus: can.BusABC, node_id: int, reply_timeout: float = 2.0) -> None:
        tx, rx = boot_ids(node_id)
        self.node_id = node_id
        self.link = IsoTpLink(bus, tx, rx)
        self.reply_timeout = reply_timeout

    def _call(self, cmd: int, body: bytes = b"", timeout: float | None = None) -> tuple[int, bytes]:
        self.link.send(bytes([cmd]) + body)
        deadline_timeout = self.reply_timeout if timeout is None else timeout
        while True:
            reply = self.link.recv(deadline_timeout)
            if len(reply) >= 2 and reply[0] == cmd | 0x40:
                return reply[1], reply[2:]

    def _ok(self, cmd: int, body: bytes = b"", timeout: float | None = None) -> bytes:
        status, extra = self._call(cmd, body, timeout)
        if status != OK:
            name = STATUS_NAMES.get(status, f"status {status}")
            raise FlashError(f"node {self.node_id} refused: {name}" + (f" ({STATUS_HELP[status]})" if status in STATUS_HELP else ""))
        return extra

    def info(self) -> NodeInfo:
        try:
            extra = self._ok(INFO)
        except IsoTpTimeout as exc:
            raise FlashError(f"node {self.node_id} doesn't answer update requests (is it running the bench sketch with CAN updates?)") from exc
        if len(extra) < 5:
            raise FlashError("short INFO reply")
        return NodeInfo(extra[0], extra[1], extra[2], (extra[3] | extra[4] << 8) * 1024)

    def abort(self) -> None:
        # Best effort: if this doesn't get through, the node gives up by itself after 30 s.
        with contextlib.suppress(IsoTpError, can.CanError, OSError):
            self._call(ABORT, timeout=0.5)

    def flash(self, image: FirmwareImage, progress: Callable[[str, float], None] = lambda phase, pct: None) -> None:
        progress("check", 0.0)
        info = self.info()
        if info.hw_target != image.hw_target:
            raise FlashError(f"node {self.node_id} is an {info.target_name}, but the image is for an {image.target_name}")
        if image.size > info.slot_bytes:
            raise FlashError(f"image is {image.size} bytes; node {self.node_id}'s slot holds {info.slot_bytes}")
        try:
            self._ok(BEGIN, struct.pack("<IIB", image.size, image.crc32, image.hw_target), timeout=5.0)
            offset = 0
            while offset < image.size:
                offset = self._write_block(image.data, offset)
                progress("write", offset / image.size)
            progress("verify", 1.0)
            self._ok(VERIFY, timeout=10.0)
            self._ok(COMMIT, timeout=5.0)
            progress("boot", 1.0)
            self._ok(BOOT)
        except BaseException:
            self.abort()
            raise

    def _write_block(self, data: bytes, offset: int) -> int:
        block = data[offset : offset + BLOCK]
        for _ in range(WRITE_RETRIES):
            try:
                status, extra = self._call(WRITE, struct.pack("<I", offset) + block, timeout=3.0)
            except IsoTpTimeout:
                continue  # a frame got lost: send the block again
            next_offset = struct.unpack_from("<I", extra)[0] if len(extra) >= 4 else None
            if status == OK and next_offset == offset + len(block):
                return next_offset
            if status == BAD_ARG and next_offset == offset + len(block):
                return next_offset  # our earlier copy landed but its reply was lost
            if status != BAD_ARG:
                raise FlashError(f"node {self.node_id} refused block at {offset}: {STATUS_NAMES.get(status, status)}")
        raise FlashError(f"node {self.node_id} stopped answering at byte {offset}")
