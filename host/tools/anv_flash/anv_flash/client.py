"""One bootloader conversation over ISO-TP (can-isotp) on a node's class-7 channel."""

from __future__ import annotations

import time

import can
import isotp

from . import protocol as p
from .bus import CLASS_BOOTLOADER, can_id


class BootloaderTimeout(TimeoutError):
    """No response in time (node not in its bootloader, or busy)."""


class BootloaderClient:
    """Requests to the bootloader listening on `channel_node`'s class-7 pair (sub 0 out, sub 1 in).

    `channel_node` is the node's own ID, or 31 for the rescue channel.
    """

    def __init__(self, bus: can.BusABC, notifier: can.Notifier, channel_node: int) -> None:
        self.channel_node = channel_node
        address = isotp.Address(
            isotp.AddressingMode.Normal_11bits,
            txid=can_id(CLASS_BOOTLOADER, channel_node, 0),
            rxid=can_id(CLASS_BOOTLOADER, channel_node, 1),
        )
        params = {
            "tx_padding": 0xAA,  # full 8-byte frames, as the node's isotp-c sends
            "blocking_send": True,
            "rx_flowcontrol_timeout": 1000,
            "rx_consecutive_frame_timeout": 1000,
            "max_frame_size": 4095,
        }
        self._stack = isotp.NotifierBasedCanStack(bus, notifier, address=address, params=params)
        self._stack.start()

    def close(self) -> None:
        self._stack.stop()

    def __enter__(self) -> BootloaderClient:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def request(self, payload: bytes, timeout: float = 1.0) -> p.Response:
        while self._stack.available():  # a late reply to an earlier, timed-out request
            self._stack.recv()
        try:
            self._stack.send(payload, send_timeout=max(timeout, 2.0))
        except isotp.BlockingSendFailure as e:
            raise BootloaderTimeout(f"node {self.channel_node}: send failed ({e})") from e
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            data = self._stack.recv(block=True, timeout=max(remaining, 0.0))
            if data is None:
                raise BootloaderTimeout(f"node {self.channel_node}: no response to command 0x{payload[0]:02X}")
            resp = p.parse_response(bytes(data))
            if resp.command == payload[0]:
                return resp

    def info(self, timeout: float = 1.0) -> p.Info:
        resp = self.request(p.request_info(), timeout)
        if not resp.ok:
            raise p.ProtocolError(f"INFO failed: {resp.status_name}")
        return p.parse_info(resp.payload)

    def erase(self, offset: int, length: int, timeout: float = 5.0) -> p.Response:
        # An F4 128 KB sector takes 1-2 s to erase (docs/architecture.md): wait up to 5 s.
        return self.request(p.request_erase(offset, length), timeout)

    def write(self, offset: int, data: bytes, timeout: float = 2.0) -> p.Response:
        return self.request(p.request_write(offset, data), timeout)

    def verify(self, crc32: int, timeout: float = 3.0) -> p.Response:
        return self.request(p.request_verify(crc32), timeout)

    def commit(self, timeout: float = 3.0) -> p.Response:
        return self.request(p.request_commit(), timeout)

    def boot(self, timeout: float = 3.0) -> p.Response:
        return self.request(p.request_boot(), timeout)

    def abort(self, timeout: float = 10.0) -> p.Response:
        return self.request(p.request_abort(), timeout)

    def select(self, uid32: int, timeout: float = 0.1) -> tuple[int, int] | None:
        """(node ID, uid32) if the board with this UID answered, else None."""
        try:
            resp = self.request(p.request_select(uid32), timeout)
        except BootloaderTimeout:
            return None
        return p.parse_select(resp.payload) if resp.ok else None
