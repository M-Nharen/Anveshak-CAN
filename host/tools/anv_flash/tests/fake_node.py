"""A simulated STM32 node for anv-flash tests: application mode (heartbeats, NMT
ENTER_BOOTLOADER) and bootloader mode (boot window, ISO-TP command set), on a python-can
virtual bus. The bootloader half mirrors firmware/bootloader_stm32/src/anv_bl_cmd.c; the C
code itself is tested by firmware/bootloader_stm32/tests/.
"""

from __future__ import annotations

import queue
import struct
import threading
import time
import zlib

import can
import cantools
import isotp

from anv_flash import protocol as p
from anv_flash.bus import (
    CLASS_BOOTLOADER,
    NMT_ENTER_BOOTLOADER,
    NODE_UNCONFIGURED,
    ROLE_MASTER,
    STATE_BOOTLOADER,
    STATE_DISARMED,
    can_id,
)
from anv_flash.image import HEADER_LEN, HEADER_SLOT, ImageHeader

ERR_APP_CRC_FAIL = 0x0701
ERR_WRONG_NODE_TYPE = 0x0702
ERR_FLASH_WRITE_FAIL = 0x0605
ERR_BAD_FRAME = 0x0802

PAGE = 2048
PAGES = 46
ALIGN = 8
APP_BASE = 0x08009000


class FakeFlash:
    def __init__(self) -> None:
        self.mem = bytearray(b"\xff" * (PAGE * PAGES))
        self.fail_writes_after: int | None = None  # simulate a flash failure after N programs
        self.programs = 0

    def erase_unit(self, i: int) -> None:
        self.mem[i * PAGE : (i + 1) * PAGE] = b"\xff" * PAGE

    def program(self, off: int, data: bytes) -> bool:
        self.programs += 1
        if self.fail_writes_after is not None and self.programs > self.fail_writes_after:
            return False
        if any(b != 0xFF for b in self.mem[off : off + len(data)]):
            return False
        self.mem[off : off + len(data)] = data
        return True

    def app_check(self, node_type: int) -> int:
        hdr = ImageHeader.unpack(bytes(self.mem[:HEADER_LEN]))
        if not hdr.header_ok():
            return ERR_APP_CRC_FAIL
        if hdr.node_type != node_type:
            return ERR_WRONG_NODE_TYPE
        if hdr.image_length == 0 or hdr.image_length > len(self.mem) - HEADER_SLOT:
            return ERR_APP_CRC_FAIL
        body = bytes(self.mem[HEADER_SLOT : HEADER_SLOT + hdr.image_length])
        return 0 if zlib.crc32(body) == hdr.image_crc32 else ERR_APP_CRC_FAIL

    def header(self) -> ImageHeader:
        return ImageHeader.unpack(bytes(self.mem[:HEADER_LEN]))


class BootloaderLogic:
    """Python twin of anv_bl_cmd.c."""

    def __init__(self, flash: FakeFlash, node_id: int, node_type: int, uid: bytes) -> None:
        self.flash = flash
        self.node_id = node_id
        self.node_type = node_type
        self.uid = uid
        self.uid32 = zlib.crc32(uid)
        self.selected = False
        self.boot_requested = False
        self._reset_session()

    def _reset_session(self) -> None:
        self.header_raw: bytes | None = None
        self.verified = False
        self.touched_end = 0

    @staticmethod
    def _resp(cmd: int, status: int, err: int = 0, payload: bytes = b"") -> bytes:
        return struct.pack("<BBH", cmd, status, 0 if status == p.ST_OK else err) + payload

    def handle(self, req: bytes) -> bytes | None:
        cmd = req[0]
        if cmd == p.CMD_INFO:
            payload = struct.pack("<HBB", p.BL_VERSION, self.node_id, self.node_type) + self.uid
            payload += struct.pack(
                "<IHIIHBHHB", self.uid32, 128, APP_BASE, PAGE * PAGES, HEADER_SLOT, ALIGN, p.MAX_WRITE,
                self.flash.app_check(self.node_type), 1,
            )
            payload += struct.pack("<HI", PAGES, PAGE) + bytes(self.flash.mem[:HEADER_LEN])
            return self._resp(cmd, p.ST_OK, payload=payload)
        if cmd == p.CMD_ERASE:
            off, length = struct.unpack_from("<II", req, 1)
            if length == 0 or off >= len(self.flash.mem) or length > len(self.flash.mem) - off:
                return self._resp(cmd, p.ST_BAD_RANGE, ERR_BAD_FRAME)
            for i in range(off // PAGE, (off + length + PAGE - 1) // PAGE):
                self.flash.erase_unit(i)
            self.verified = False
            self.touched_end = max(self.touched_end, off + length)
            return self._resp(cmd, p.ST_OK)
        if cmd == p.CMD_WRITE:
            (off,) = struct.unpack_from("<I", req, 1)
            data = bytes(req[5:])
            if off % ALIGN or off + len(data) > len(self.flash.mem):
                return self._resp(cmd, p.ST_BAD_RANGE, ERR_BAD_FRAME)
            if off < HEADER_SLOT:
                if off != 0 or len(data) < HEADER_LEN:
                    return self._resp(cmd, p.ST_BAD_RANGE, ERR_BAD_FRAME)
                hdr = ImageHeader.unpack(data)
                if not hdr.header_ok():
                    return self._resp(cmd, p.ST_BAD_IMAGE, ERR_APP_CRC_FAIL)
                if hdr.node_type != self.node_type:
                    return self._resp(cmd, p.ST_WRONG_NODE_TYPE, ERR_WRONG_NODE_TYPE)
                self.header_raw = data[:HEADER_LEN]
                self.verified = False
                skip = min(len(data), HEADER_SLOT)
                off, data = off + skip, data[skip:]
                if not data:
                    return self._resp(cmd, p.ST_OK)
            padded = data + b"\xff" * (-len(data) % ALIGN)
            self.verified = False
            self.touched_end = max(self.touched_end, off + len(data))
            if not self.flash.program(off, padded):
                return self._resp(cmd, p.ST_FLASH_ERROR, ERR_FLASH_WRITE_FAIL)
            return self._resp(cmd, p.ST_OK)
        if cmd == p.CMD_VERIFY:
            (expected,) = struct.unpack_from("<I", req, 1)
            if self.header_raw is None:
                return self._resp(cmd, p.ST_BAD_SEQUENCE, ERR_BAD_FRAME)
            hdr = ImageHeader.unpack(self.header_raw)
            actual = zlib.crc32(bytes(self.flash.mem[HEADER_SLOT : HEADER_SLOT + hdr.image_length]))
            self.verified = actual == expected == hdr.image_crc32
            st = p.ST_OK if self.verified else p.ST_MISMATCH
            return self._resp(cmd, st, ERR_APP_CRC_FAIL, struct.pack("<I", actual))
        if cmd == p.CMD_COMMIT:
            if not self.verified or self.header_raw is None:
                return self._resp(cmd, p.ST_BAD_SEQUENCE, ERR_BAD_FRAME)
            if not self.flash.program(0, self.header_raw):
                return self._resp(cmd, p.ST_NOT_ERASED, ERR_FLASH_WRITE_FAIL)
            self.verified = False
            err = self.flash.app_check(self.node_type)
            return self._resp(cmd, p.ST_OK if err == 0 else p.ST_BAD_IMAGE, err)
        if cmd == p.CMD_BOOT:
            err = self.flash.app_check(self.node_type)
            if err:
                return self._resp(cmd, p.ST_BAD_IMAGE, err)
            self.boot_requested = True
            return self._resp(cmd, p.ST_OK)
        if cmd == p.CMD_ABORT:
            for i in range((self.touched_end + PAGE - 1) // PAGE):
                self.flash.erase_unit(i)
            self._reset_session()
            return self._resp(cmd, p.ST_OK)
        if cmd == p.CMD_SELECT:
            (uid32,) = struct.unpack_from("<I", req, 1)
            self.selected = uid32 == self.uid32
            if not self.selected:
                return None
            return self._resp(cmd, p.ST_OK, payload=struct.pack("<BI", self.node_id, self.uid32))
        return self._resp(cmd, p.ST_UNKNOWN_COMMAND, ERR_BAD_FRAME)


class FakeNode:
    """One node on a virtual bus, in its own thread.

    crash_build: if the application in flash has this build ID, it "crashes" straight after
    the boot window, so the node is only ever reachable during its 300 ms window.
    """

    def __init__(
        self,
        channel: str,
        db: cantools.database.can.Database,
        node_id: int,
        uid: bytes,
        node_type: int = 1,
        image: bytes | None = None,
        state: int = STATE_DISARMED,
        start_in_bootloader: bool = False,
        crash_build: int | None = None,
        report_build: int | None = None,
        listen_s: float = 0.3,
        hb_period_s: float = 0.05,
    ) -> None:
        self.db = db
        self.node_id = node_id
        self.node_type = node_type
        self.flash = FakeFlash()
        if image is not None:
            self.flash.mem[: len(image)] = image
        self.app_state = state
        self.crash_build = crash_build
        self.report_build = report_build
        self.listen_s = listen_s
        self.hb_period_s = hb_period_s
        self.logic = BootloaderLogic(self.flash, node_id, node_type, uid)
        self.history: list[str] = []  # "app", "bootloader" transitions, in order
        self.master: tuple[int, int] | None = None

        self.bus = can.Bus(interface="virtual", channel=channel, receive_own_messages=False)
        self.notifier = can.Notifier(self.bus, [], timeout=0.01)
        self._frames: queue.Queue[can.Message] = queue.Queue()
        self.notifier.add_listener(self._on_frame)
        self._stacks: list[tuple[isotp.NotifierBasedCanStack, bool]] = []
        for chan_node, needs_select in self._channels():
            addr = isotp.Address(
                isotp.AddressingMode.Normal_11bits,
                txid=can_id(CLASS_BOOTLOADER, chan_node, 1),
                rxid=can_id(CLASS_BOOTLOADER, chan_node, 0),
            )
            params = {"tx_padding": 0xAA, "max_frame_size": 8192, "blocksize": 8, "stmin": 0}
            stack = isotp.NotifierBasedCanStack(self.bus, self.notifier, address=addr, params=params)
            stack.start()
            self._stacks.append((stack, needs_select))

        self._hb = db.get_message_by_name("DRIVE1_NODE_HB")
        self._nmt = db.get_message_by_name("NMT")
        self._host_hb = {db.get_message_by_name(n).frame_id: i for i, n in
                         ((1, "HOST_HB_ORIN"), (2, "HOST_HB_RPI"), (3, "HOST_HB_TOOL"))}
        self._stop = threading.Event()
        self._boot(stay=start_in_bootloader)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _channels(self) -> list[tuple[int, bool]]:
        if self.node_id == NODE_UNCONFIGURED:
            return [(NODE_UNCONFIGURED, True)]
        return [(self.node_id, False), (NODE_UNCONFIGURED, True)]

    # ---- life cycle ----------------------------------------------------------------------

    def _boot(self, stay: bool) -> None:
        """A reset: into the bootloader, which stays (flag) or listens for its window."""
        self.mode = "bootloader" if stay else "listen"
        self.boot_at = time.monotonic()
        self.logic.boot_requested = False
        self.logic.selected = False
        self.next_hb = 0.0
        self.history.append("bootloader")

    def _start_app(self) -> None:
        build = self.flash.header().build_id
        if self.crash_build is not None and build == self.crash_build:
            self._boot(stay=False)  # crashes at once: back through the bootloader
            return
        self.mode = "app"
        self.app_state = STATE_DISARMED if self.app_state == STATE_BOOTLOADER else self.app_state
        self.history.append("app")

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2)
        for stack, _ in self._stacks:
            stack.stop()
        self.notifier.stop()
        self.bus.shutdown()

    @property
    def build16(self) -> int:
        if self.report_build is not None:
            return self.report_build & 0xFFFF
        return self.flash.header().build_id & 0xFFFF if self.flash.app_check(self.node_type) == 0 else 0

    # ---- loop ------------------------------------------------------------------------------

    def _on_frame(self, msg: can.Message) -> None:
        if msg.arbitration_id == self._nmt.frame_id or msg.arbitration_id in self._host_hb:
            self._frames.put(msg)

    def _send_hb(self, state: int) -> None:
        data = self._hb.encode({
            "STATE": state, "NODE_TYPE": self.node_type, "PROTOCOL_VERSION": 1, "CAN_ERROR_STATE": 0,
            "BOOTLOADER_RUNNING": int(state == STATE_BOOTLOADER), "UNSAVED_PARAM_CHANGES": 0,
            "DEBUG_LEVEL": 0, "FW_BUILD_ID": self.build16,
            "UPTIME_S": 0,
        })
        self.bus.send(can.Message(arbitration_id=can_id(6, self.node_id, 0), data=data, is_extended_id=False))

    def _handle_network(self) -> None:
        while True:
            try:
                msg = self._frames.get_nowait()
            except queue.Empty:
                return
            if msg.arbitration_id in self._host_hb:
                d = self.db.get_message_by_frame_id(msg.arbitration_id).decode(bytes(msg.data), decode_choices=False)
                if d["ROLE"] == ROLE_MASTER:
                    self.master = (self._host_hb[msg.arbitration_id], int(d["TERM"]) & 0xFF)
                continue
            if self.mode != "app":
                continue
            d = self._nmt.decode(bytes(msg.data), decode_choices=False)
            if d["TARGET_NODE"] not in (0, self.node_id) or (d["MASTER_NODE_ID"], d["MASTER_TERM"]) != self.master:
                continue
            if d["COMMAND"] == NMT_ENTER_BOOTLOADER and self.app_state == STATE_DISARMED:
                self._boot(stay=True)

    def _serve(self) -> None:
        for stack, needs_select in self._stacks:
            while stack.available():
                req = bytes(stack.recv())
                if self.mode == "app" or not req:
                    continue  # the application ignores class 7
                if needs_select and not self.logic.selected and req[0] != p.CMD_SELECT:
                    continue
                resp = self.logic.handle(req)
                if resp is None:
                    continue
                stack.send(resp)
                if self.mode == "listen":
                    self.mode = "bootloader"

    def _run(self) -> None:
        while not self._stop.is_set():
            now = time.monotonic()
            self._handle_network()
            self._serve()
            if self.mode == "listen" and now - self.boot_at >= self.listen_s:
                if self.flash.app_check(self.node_type) == 0:
                    self._start_app()
                else:
                    self.mode = "bootloader"
            if self.logic.boot_requested:
                time.sleep(0.02)
                self._boot(stay=False)
            if self.mode in ("app", "bootloader") and now >= self.next_hb:
                self.next_hb = now + self.hb_period_s
                self._send_hb(self.app_state if self.mode == "app" else STATE_BOOTLOADER)
            time.sleep(0.001)
