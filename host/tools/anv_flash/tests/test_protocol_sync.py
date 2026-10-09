"""anv_flash.protocol must match firmware/bootloader_stm32/include/anv_bl_proto.h."""

from __future__ import annotations

import re
from pathlib import Path

from anv_flash import image, protocol

HEADER = Path(__file__).resolve().parents[4] / "firmware" / "bootloader_stm32" / "include" / "anv_bl_proto.h"


def _c_defines() -> dict[str, int]:
    out = {}
    for name, value in re.findall(r"^#define (ANV_BL_\w+) (0x[0-9A-Fa-f]+|\d+)u?\b", HEADER.read_text(), re.M):
        out[name] = int(value, 0)
    return out


def test_constants_match_c_header():
    c = _c_defines()
    pairs = {
        "ANV_BL_VERSION": protocol.BL_VERSION,
        "ANV_BL_RESP_PREFIX_LEN": protocol.RESP_PREFIX_LEN,
        "ANV_BL_MAX_WRITE": protocol.MAX_WRITE,
        "ANV_BL_WRITE_OVERHEAD": protocol.WRITE_OVERHEAD,
        "ANV_BL_INFO_FIXED_LEN": protocol.INFO_FIXED_LEN,
        "ANV_BL_STAY_FLAG_MAGIC": protocol.STAY_FLAG_MAGIC,
        "ANV_BL_HEADER_MAGIC": image.HEADER_MAGIC,
        "ANV_BL_HEADER_VERSION": image.HEADER_VERSION,
        "ANV_BL_HEADER_LEN": image.HEADER_LEN,
        "ANV_BL_HEADER_SLOT": image.HEADER_SLOT,
        "ANV_BL_HEADER_CRC_OFFSET": image.HEADER_CRC_OFFSET,
    }
    for name in c:
        if name.startswith("ANV_BL_CMD_"):
            pairs[name] = getattr(protocol, "CMD_" + name.removeprefix("ANV_BL_CMD_"))
        elif name.startswith("ANV_BL_ST_"):
            pairs[name] = getattr(protocol, "ST_" + name.removeprefix("ANV_BL_ST_"))
    assert len([n for n in pairs if n.startswith("ANV_BL_CMD_")]) == 8
    assert len([n for n in pairs if n.startswith("ANV_BL_ST_")]) == 10
    for name, py_value in pairs.items():
        assert c[name] == py_value, name
