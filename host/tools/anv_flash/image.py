"""Firmware images: read an Arduino/ESP-IDF app .bin and check it before anything goes on the bus."""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

ESP_IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 0x20  # after the 24-byte image header and the first 8-byte segment header
ELF_SHA_OFFSET = 0xB0  # esptool --elf-sha256-offset 0xb0, as the Arduino core builds
# esp_image_header_t chip_id -> NODE_ID_REQUEST HW_TARGET
CHIP_TO_HW_TARGET = {0: 0, 9: 1}
HW_TARGET_NAMES = {0: "ESP32 (WROOM)", 1: "ESP32-S3"}


class ImageError(ValueError):
    pass


@dataclass(frozen=True)
class FirmwareImage:
    data: bytes
    hw_target: int  # 0 ESP32, 1 ESP32-S3
    build_id: int  # what the node's heartbeat FW_BUILD_ID will show once this image runs
    project: str

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def crc32(self) -> int:
        return zlib.crc32(self.data) & 0xFFFFFFFF

    @property
    def target_name(self) -> str:
        return HW_TARGET_NAMES.get(self.hw_target, f"target {self.hw_target}")


def parse_image(data: bytes, name: str = "image") -> FirmwareImage:
    if len(data) < ELF_SHA_OFFSET + 2:
        raise ImageError(f"{name} is too small ({len(data)} bytes) to be an app image")
    if data[0] != ESP_IMAGE_MAGIC or struct.unpack_from("<I", data, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        hint = ""
        if len(data) >= 0x10000 and data[0] in (0xFF, ESP_IMAGE_MAGIC):
            hint = " It looks like a merged/full-flash image: use the '.ino.bin' file, not '.ino.merged.bin'."
        raise ImageError(f"{name} is not an ESP32 app image.{hint}")
    chip_id = struct.unpack_from("<H", data, 12)[0]
    if chip_id not in CHIP_TO_HW_TARGET:
        raise ImageError(f"{name} is built for chip id {chip_id}; only the ESP32 (0) and ESP32-S3 (9) are supported")
    project = data[APP_DESC_OFFSET + 48 : APP_DESC_OFFSET + 80].split(b"\0")[0].decode("ascii", "replace")
    return FirmwareImage(
        data=bytes(data),
        hw_target=CHIP_TO_HW_TARGET[chip_id],
        build_id=data[ELF_SHA_OFFSET] | (data[ELF_SHA_OFFSET + 1] << 8),
        project=project,
    )


def load_image(path: str | Path) -> FirmwareImage:
    p = Path(path)
    return parse_image(p.read_bytes(), p.name)
