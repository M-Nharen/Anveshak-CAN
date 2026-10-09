"""STM32 application image: the 512-byte header slot followed by the firmware body.

Layout and constants match firmware/bootloader_stm32/include/anv_bl_proto.h. Standard library
only, because the firmware build runs this file directly to stamp freshly linked images:

    python image.py stamp drive_raw.bin drive.bin --git-dir <repo>
    python image.py show drive.bin
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass, replace
from pathlib import Path

HEADER_MAGIC = 0x48564E41  # "ANVH"
HEADER_VERSION = 1
HEADER_LEN = 64
HEADER_SLOT = 0x200
HEADER_CRC_OFFSET = 60

# magic, header_version, node_type, reserved0, image_length, image_crc32, fw_version,
# build_id, git_hash[16], reserved1[20], header_crc32
_HEADER_FMT = "<IHBBIIII16s20sI"
assert struct.calcsize(_HEADER_FMT) == HEADER_LEN


class ImageError(ValueError):
    """The file is not a valid, complete application image."""


def crc32(data: bytes) -> int:
    """CRC-32/ISO-HDLC, as the bootloader computes it."""
    return zlib.crc32(data) & 0xFFFFFFFF


@dataclass(frozen=True)
class ImageHeader:
    magic: int
    header_version: int
    node_type: int
    image_length: int
    image_crc32: int
    fw_version: int
    build_id: int
    git_hash: str
    header_crc32: int

    @classmethod
    def unpack(cls, raw: bytes) -> ImageHeader:
        if len(raw) < HEADER_LEN:
            raise ImageError(f"header needs {HEADER_LEN} bytes, got {len(raw)}")
        f = struct.unpack(_HEADER_FMT, raw[:HEADER_LEN])
        git = f[8].split(b"\0", 1)[0].split(b"\xff", 1)[0].decode("ascii", "replace")
        return cls(f[0], f[1], f[2], f[4], f[5], f[6], f[7], git, f[10])

    def pack_unsealed(self) -> bytes:
        """The first 60 bytes (everything the header CRC covers)."""
        git = self.git_hash.encode("ascii")[:16].ljust(16, b"\0")
        full = struct.pack(
            _HEADER_FMT,
            self.magic,
            self.header_version,
            self.node_type,
            0,
            self.image_length,
            self.image_crc32,
            self.fw_version,
            self.build_id,
            git,
            bytes(20),
            0,
        )
        return full[:HEADER_CRC_OFFSET]

    def sealed(self) -> ImageHeader:
        return replace(self, header_crc32=crc32(self.pack_unsealed()))

    def pack(self) -> bytes:
        return self.pack_unsealed() + struct.pack("<I", self.header_crc32)

    def header_ok(self) -> bool:
        return (
            self.magic == HEADER_MAGIC
            and self.header_version == HEADER_VERSION
            and crc32(self.pack_unsealed()) == self.header_crc32
        )

    @property
    def build_id16(self) -> int:
        """What NODE_HB's FW_BUILD_ID shows for this image."""
        return self.build_id & 0xFFFF

    @property
    def version_str(self) -> str:
        v = self.fw_version
        return f"{(v >> 16) & 0xFFFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"


@dataclass(frozen=True)
class FirmwareImage:
    data: bytes
    header: ImageHeader

    @property
    def body(self) -> bytes:
        return self.data[HEADER_SLOT : HEADER_SLOT + self.header.image_length]

    @classmethod
    def from_bytes(cls, data: bytes) -> FirmwareImage:
        if len(data) <= HEADER_SLOT:
            raise ImageError("file is too short to hold a header and a body")
        hdr = ImageHeader.unpack(data)
        if hdr.magic != HEADER_MAGIC:
            raise ImageError("no application header (wrong magic): was this image stamped?")
        if hdr.header_version != HEADER_VERSION:
            raise ImageError(f"unsupported header version {hdr.header_version}")
        if not hdr.header_ok():
            raise ImageError("header CRC mismatch")
        if hdr.image_length != len(data) - HEADER_SLOT:
            raise ImageError(f"header says {hdr.image_length} body bytes, file has {len(data) - HEADER_SLOT}")
        if crc32(data[HEADER_SLOT:]) != hdr.image_crc32:
            raise ImageError("image CRC mismatch")
        return cls(bytes(data), hdr)

    @classmethod
    def load(cls, path: str | Path) -> FirmwareImage:
        return cls.from_bytes(Path(path).read_bytes())


def stamp(raw: bytes, build_id: int, git_hash: str) -> bytes:
    """Fill in length, CRCs, build ID and git hash of a freshly linked image.

    `raw` is objcopy's binary output, starting at the header slot, with the header's magic,
    version, node type and firmware version already set by the C source.
    """
    if len(raw) <= HEADER_SLOT:
        raise ImageError("linked image is too short")
    hdr = ImageHeader.unpack(raw)
    if hdr.magic != HEADER_MAGIC or hdr.header_version != HEADER_VERSION:
        raise ImageError("linked image does not start with an application header")
    body = bytes(raw[HEADER_SLOT:])
    hdr = replace(
        hdr,
        image_length=len(body),
        image_crc32=crc32(body),
        build_id=build_id & 0xFFFFFFFF,
        git_hash=git_hash[:16],
    ).sealed()
    slot = hdr.pack() + b"\xff" * (HEADER_SLOT - HEADER_LEN)
    return slot + body


def git_build_info(git_dir: str | Path | None) -> tuple[int, str]:
    """(build ID, short hash) of the checkout: the build ID is the first 32 bits of the commit."""
    try:
        full = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=git_dir,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return 0, "unknown"
    return int(full[:8], 16), full[:12]


def _main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_stamp = sub.add_parser("stamp", help="fill in a linked image's header")
    p_stamp.add_argument("raw", type=Path)
    p_stamp.add_argument("out", type=Path)
    p_stamp.add_argument("--git-dir", type=Path, default=None)
    p_stamp.add_argument("--build-id", type=lambda s: int(s, 0), default=None)
    p_show = sub.add_parser("show", help="print an image's header")
    p_show.add_argument("image", type=Path)
    args = parser.parse_args(argv)

    try:
        if args.cmd == "stamp":
            build_id, git_hash = git_build_info(args.git_dir)
            if args.build_id is not None:
                build_id = args.build_id
            args.out.write_bytes(stamp(args.raw.read_bytes(), build_id, git_hash))
            img = FirmwareImage.load(args.out)
        else:
            img = FirmwareImage.load(args.image)
    except ImageError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    h = img.header
    print(
        f"node_type={h.node_type} version={h.version_str} length={h.image_length} "
        f"crc32=0x{h.image_crc32:08X} build_id=0x{h.build_id:08X} git={h.git_hash}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(_main())
