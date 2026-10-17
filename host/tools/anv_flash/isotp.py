"""ISO-TP (ISO 15765-2) over python-can, sender side, for 11-bit IDs and 8-byte classic frames.

The node answers with single frames only, so receiving needs nothing more. Written here rather than
taken from a library so the same code runs on SocketCAN and on python-can's virtual bus in tests."""

from __future__ import annotations

import time

import can

PAD = 0xAA
FC_CTS, FC_WAIT, FC_OVERFLOW = 0, 1, 2


class IsoTpError(Exception):
    pass


class IsoTpTimeout(IsoTpError):
    pass


def stmin_seconds(code: int) -> float:
    if code <= 0x7F:
        return code / 1000.0
    if 0xF1 <= code <= 0xF9:
        return (code - 0xF0) / 10000.0
    return 0.127  # reserved values mean "use the maximum"


class IsoTpLink:
    def __init__(self, bus: can.BusABC, tx_id: int, rx_id: int, fc_timeout: float = 1.0) -> None:
        self.bus = bus
        self.tx_id = tx_id
        self.rx_id = rx_id
        self.fc_timeout = fc_timeout

    def _frame(self, data: bytes) -> None:
        msg = can.Message(arbitration_id=self.tx_id, is_extended_id=False, data=data.ljust(8, bytes([PAD])))
        deadline = time.monotonic() + 0.5
        while True:
            try:
                self.bus.send(msg)
                return
            except can.CanOperationError:
                # The interface's transmit queue is full: give the controller a moment to drain it.
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.001)

    def _next_from_node(self, deadline: float) -> bytes | None:
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            msg = self.bus.recv(timeout=left)
            if msg is not None and msg.arbitration_id == self.rx_id and not msg.is_extended_id and msg.dlc:
                return bytes(msg.data)

    def _wait_flow_control(self) -> tuple[int, float]:
        deadline = time.monotonic() + self.fc_timeout
        while True:
            data = self._next_from_node(deadline)
            if data is None:
                raise IsoTpTimeout("no flow control from the node")
            if data[0] >> 4 != 3:
                continue
            status = data[0] & 0x0F
            if status == FC_CTS:
                return data[1], stmin_seconds(data[2])
            if status == FC_WAIT:
                deadline = time.monotonic() + self.fc_timeout
                continue
            raise IsoTpError("the node has no room for a message this long")

    def send(self, payload: bytes) -> None:
        n = len(payload)
        if n == 0 or n > 4095:
            raise ValueError("ISO-TP payload must be 1-4095 bytes")
        if n <= 7:
            self._frame(bytes([n]) + payload)
            return
        self._frame(bytes([0x10 | (n >> 8), n & 0xFF]) + payload[:6])
        block_size, stmin = self._wait_flow_control()
        index, sn, in_block = 6, 1, 0
        while index < n:
            if block_size and in_block == block_size:
                block_size, stmin = self._wait_flow_control()
                in_block = 0
            self._frame(bytes([0x20 | sn]) + payload[index : index + 7])
            index += 7
            sn = (sn + 1) & 0x0F
            in_block += 1
            if stmin and index < n:
                time.sleep(stmin)

    def recv(self, timeout: float) -> bytes:
        """The next single-frame message from the node."""
        deadline = time.monotonic() + timeout
        while True:
            data = self._next_from_node(deadline)
            if data is None:
                raise IsoTpTimeout("no reply from the node")
            if data[0] >> 4 == 0 and 1 <= (data[0] & 0x0F) <= 7:
                return data[1 : 1 + (data[0] & 0x0F)]
