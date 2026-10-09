"""Bootloader request/response payloads (carried over ISO-TP on class 7).

Mirrors firmware/bootloader_stm32/include/anv_bl_proto.h; tests/test_protocol_sync.py parses
that header and fails if the two drift apart.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .image import HEADER_LEN, ImageHeader

BL_VERSION = 0x0100

CMD_INFO = 0x01
CMD_ERASE = 0x02
CMD_WRITE = 0x03
CMD_VERIFY = 0x04
CMD_COMMIT = 0x05
CMD_BOOT = 0x06
CMD_ABORT = 0x07
CMD_SELECT = 0x08

ST_OK = 0
ST_MISMATCH = 1
ST_BAD_LENGTH = 2
ST_BAD_RANGE = 3
ST_NOT_ERASED = 4
ST_FLASH_ERROR = 5
ST_WRONG_NODE_TYPE = 6
ST_BAD_IMAGE = 7
ST_BAD_SEQUENCE = 8
ST_UNKNOWN_COMMAND = 9

STATUS_NAMES = {
    ST_OK: "OK",
    ST_MISMATCH: "MISMATCH",
    ST_BAD_LENGTH: "BAD_LENGTH",
    ST_BAD_RANGE: "BAD_RANGE",
    ST_NOT_ERASED: "NOT_ERASED",
    ST_FLASH_ERROR: "FLASH_ERROR",
    ST_WRONG_NODE_TYPE: "WRONG_NODE_TYPE",
    ST_BAD_IMAGE: "BAD_IMAGE",
    ST_BAD_SEQUENCE: "BAD_SEQUENCE",
    ST_UNKNOWN_COMMAND: "UNKNOWN_COMMAND",
}

RESP_PREFIX_LEN = 4
MAX_WRITE = 4096
WRITE_OVERHEAD = 5
MAX_REQUEST = MAX_WRITE + WRITE_OVERHEAD
INFO_FIXED_LEN = 38
STAY_FLAG_MAGIC = 0x424C5354


class ProtocolError(ValueError):
    """A response that doesn't parse."""


@dataclass(frozen=True)
class Response:
    command: int
    status: int
    error_code: int
    payload: bytes

    @property
    def ok(self) -> bool:
        return self.status == ST_OK

    @property
    def status_name(self) -> str:
        return STATUS_NAMES.get(self.status, f"STATUS_{self.status}")


@dataclass(frozen=True)
class Info:
    bl_version: int
    node_id: int
    node_type: int
    uid: bytes
    uid32: int
    flash_size_kb: int
    app_base: int
    region_size: int
    header_slot: int
    write_align: int
    max_write: int
    boot_status: int
    layout: tuple[tuple[int, int], ...]  # (count, size) erase-unit groups
    header: ImageHeader

    def erase_units(self) -> list[tuple[int, int]]:
        """(offset, size) of every erase unit in the application region, in order."""
        units = []
        offset = 0
        for count, size in self.layout:
            for _ in range(count):
                units.append((offset, size))
                offset += size
        return units


def request_info() -> bytes:
    return bytes([CMD_INFO])


def request_erase(offset: int, length: int) -> bytes:
    return struct.pack("<BII", CMD_ERASE, offset, length)


def request_write(offset: int, data: bytes) -> bytes:
    if not 0 < len(data) <= MAX_WRITE:
        raise ValueError(f"WRITE carries 1..{MAX_WRITE} bytes, got {len(data)}")
    return struct.pack("<BI", CMD_WRITE, offset) + bytes(data)


def request_verify(crc32: int) -> bytes:
    return struct.pack("<BI", CMD_VERIFY, crc32)


def request_commit() -> bytes:
    return bytes([CMD_COMMIT])


def request_boot() -> bytes:
    return bytes([CMD_BOOT])


def request_abort() -> bytes:
    return bytes([CMD_ABORT])


def request_select(uid32: int) -> bytes:
    return struct.pack("<BI", CMD_SELECT, uid32)


def parse_response(data: bytes) -> Response:
    if len(data) < RESP_PREFIX_LEN:
        raise ProtocolError(f"response too short ({len(data)} bytes)")
    cmd, status, err = struct.unpack_from("<BBH", data)
    return Response(cmd, status, err, bytes(data[RESP_PREFIX_LEN:]))


def parse_info(payload: bytes) -> Info:
    if len(payload) < INFO_FIXED_LEN:
        raise ProtocolError("INFO payload too short")
    bl_version, node_id, node_type = struct.unpack_from("<HBB", payload, 0)
    uid = bytes(payload[4:16])
    (uid32, flash_kb, app_base, region, slot, align, max_write, boot_status, n_groups) = struct.unpack_from(
        "<IHIIHBHHB", payload, 16
    )
    off = INFO_FIXED_LEN
    layout = []
    for _ in range(n_groups):
        if off + 6 > len(payload):
            raise ProtocolError("INFO layout truncated")
        layout.append(struct.unpack_from("<HI", payload, off))
        off += 6
    if off + HEADER_LEN > len(payload):
        raise ProtocolError("INFO header truncated")
    header = ImageHeader.unpack(payload[off : off + HEADER_LEN])
    return Info(
        bl_version, node_id, node_type, uid, uid32, flash_kb, app_base, region, slot, align, max_write,
        boot_status, tuple(layout), header,
    )


def parse_select(payload: bytes) -> tuple[int, int]:
    """(node ID, uid32) from a SELECT response."""
    if len(payload) < 5:
        raise ProtocolError("SELECT payload too short")
    node_id, uid32 = struct.unpack_from("<BI", payload)
    return node_id, uid32
