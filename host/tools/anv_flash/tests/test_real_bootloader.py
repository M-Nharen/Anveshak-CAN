"""anv-flash against the real C bootloader (isotp-c, command logic, boot sequence), built for
Linux as firmware/bootloader_stm32's `bl_stdio_node` and bridged onto the virtual bus.

Skipped unless ANV_BL_STDIO_NODE points at that executable (CI's firmware job sets it):

    cmake -S firmware -B build && cmake --build build --target bl_stdio_node
    ANV_BL_STDIO_NODE=build/bootloader_stm32/bl_stdio_node pytest host/tools/anv_flash
"""

from __future__ import annotations

import os
import queue
import subprocess
import threading
import time
import zlib

import can
import pytest

from anv_flash.bus import STATE_DISARMED, can_id
from anv_flash.image import FirmwareImage
from conftest import make_image

EXE = os.environ.get("ANV_BL_STDIO_NODE")
# Skipped only when not asked for: if ANV_BL_STDIO_NODE is set (CI), a missing binary fails.
pytestmark = pytest.mark.skipif(not EXE, reason="ANV_BL_STDIO_NODE not set")


class StdioNode:
    """Bridges bl_stdio_node to the virtual bus; plays the application after a jump."""

    def __init__(self, channel: str, db, node_id: int, uid_byte: int) -> None:
        assert EXE and os.path.exists(EXE), f"ANV_BL_STDIO_NODE={EXE} does not exist"
        self.node_id = node_id
        self.proc = subprocess.Popen(
            [EXE, str(node_id), f"{uid_byte:02X}"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
        )
        self.bus = can.Bus(interface="virtual", channel=channel, receive_own_messages=False)
        self.inbox: queue.Queue[can.Message] = queue.Queue()
        self.notifier = can.Notifier(self.bus, [self.inbox.put], timeout=0.01)
        self.hb = db.get_message_by_name("DRIVE1_NODE_HB")
        self.app_build: int | None = None
        self.jumps = 0
        self._stop = threading.Event()
        self._t0 = time.monotonic()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _exchange(self, line: str) -> None:
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        while (out := self.proc.stdout.readline().strip()) != ".":
            if out.startswith("F "):
                _, fid, data = (out.split(" ") + [""])[:3]
                self.bus.send(can.Message(arbitration_id=int(fid, 16), data=bytes.fromhex(data), is_extended_id=False))
            elif out.startswith("J "):
                self.app_build = int(out[2:], 16)
                self.jumps += 1

    def _run(self) -> None:
        next_hb = 0.0
        while not self._stop.is_set():
            ms = int((time.monotonic() - self._t0) * 1000)
            while True:
                try:
                    msg = self.inbox.get_nowait()
                except queue.Empty:
                    break
                self._exchange(f"F {msg.arbitration_id:03X} {bytes(msg.data).hex().upper()}")
            self._exchange(f"T {ms}")
            if self.app_build is not None and time.monotonic() >= next_hb:
                next_hb = time.monotonic() + 0.05
                data = self.hb.encode({
                    "STATE": STATE_DISARMED, "NODE_TYPE": 1, "PROTOCOL_VERSION": 1, "CAN_ERROR_STATE": 0,
                    "BOOTLOADER_RUNNING": 0, "UNSAVED_PARAM_CHANGES": 0, "DEBUG_LEVEL": 0,
                    "FW_BUILD_ID": self.app_build & 0xFFFF, "UPTIME_S": 0,
                })
                self.bus.send(can.Message(arbitration_id=can_id(6, self.node_id, 0), data=data, is_extended_id=False))
            time.sleep(0.0005)

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2)
        self.notifier.stop()
        self.bus.shutdown()
        self.proc.kill()
        self.proc.wait()


@pytest.fixture
def real_node(rig):
    nodes: list[StdioNode] = []

    def make(node_id: int, uid_byte: int) -> StdioNode:
        n = StdioNode(rig.channel, rig.db, node_id, uid_byte)
        nodes.append(n)
        return n

    yield make
    for n in nodes:
        n.stop()


def test_flash_real_bootloader(rig, real_node):
    node = real_node(4, 0x41)
    time.sleep(0.5)  # empty flash: the window passes and it stays, sending BOOTLOADER heartbeats
    image = FirmwareImage.from_bytes(make_image(build_id=0xC0DE1234, size=3 * 4096 + 1234, seed=3))
    flasher = rig.flasher()
    flasher.flash_node(4, image)
    assert node.app_build == 0xC0DE1234 and node.jumps == 1


def test_rescue_real_bootloader(rig, real_node):
    node = real_node(31, 0x5A)
    time.sleep(0.5)
    image = FirmwareImage.from_bytes(make_image(build_id=0x0000BEEF, size=5000, seed=4))
    uid32 = zlib.crc32(bytes([0x5A]) * 12)  # anv_bl_uid32(): CRC32 of the 96-bit UID
    assert rig.flasher().rescue(uid32, image) == 31
    assert node.app_build == 0x0000BEEF
