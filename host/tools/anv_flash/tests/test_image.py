"""Image header stamping and checks (anv_flash.image), against the bootloader's rules."""

from __future__ import annotations

import pytest

from anv_flash.image import HEADER_SLOT, FirmwareImage, ImageError, ImageHeader, crc32, stamp
from conftest import make_image


def test_crc_matches_bootloader_check_value():
    # Same check value as firmware/bootloader_stm32/tests/test_bl_cmd.c.
    assert crc32(b"123456789") == 0xCBF43926


def test_stamp_fills_header():
    data = make_image(node_type=1, build_id=0xDEADBEEF, size=1000)
    img = FirmwareImage.from_bytes(data)
    h = img.header
    assert h.node_type == 1 and h.image_length == 1000 and h.build_id == 0xDEADBEEF
    assert h.build_id16 == 0xBEEF
    assert h.image_crc32 == crc32(data[HEADER_SLOT:])
    assert h.header_ok()
    assert h.version_str == "1.2.3"
    assert data[64:HEADER_SLOT] == b"\xff" * (HEADER_SLOT - 64)


@pytest.mark.parametrize(
    "mutate, message",
    [
        (lambda d: d[:-1], "body bytes"),
        (lambda d: d[:HEADER_SLOT + 10] + bytes([d[HEADER_SLOT + 10] ^ 1]) + d[HEADER_SLOT + 11:], "image CRC"),
        (lambda d: d[:20] + bytes([d[20] ^ 1]) + d[21:], "header CRC"),
        (lambda d: b"\0" * 4 + d[4:], "magic"),
        (lambda d: d[:100], "too short"),
    ],
)
def test_rejects_broken_images(mutate, message):
    with pytest.raises(ImageError, match=message):
        FirmwareImage.from_bytes(mutate(make_image()))


def test_stamp_needs_the_c_placeholder_header():
    with pytest.raises(ImageError, match="application header"):
        stamp(b"\0" * 1024, 1, "x")


def test_header_roundtrip():
    h = ImageHeader(0x48564E41, 1, 3, 100, 5, 0x010000, 9, "abc", 0).sealed()
    assert ImageHeader.unpack(h.pack()) == h
