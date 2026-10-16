"""Test doubles: a scriptable bridge, a fake WebSocket and a sample gateway state."""

from __future__ import annotations

import asyncio
import json
from types import SimpleNamespace
from typing import Any

from starlette.websockets import WebSocketDisconnect

from anv_dashboard.bridge import BridgeError


def make_node(node_id: int, **over: Any) -> dict[str, Any]:
    n: dict[str, Any] = {
        "node_id": node_id, "name": f"node{node_id}", "seen": True, "alive": True, "age_ms": 40.0, "state": 1,
        "state_name": "DISARMED", "has_hb": True, "node_type": 1, "fw_build_id": 0xBEEF, "uptime_s": 120,
        "reboot_count": 0, "tec": 0, "rec": 0, "can_error_state": 0, "bootloader_running": False, "debug_level": 0,
        "fb": {"state": 1, "fault_flags": 0, "speed_a": 0.0, "speed_b": 0.0, "current_a": 0.0, "current_b": 0.0},
        "telem": {"supply_v": 24.0, "temp_driver_a": 30, "temp_driver_b": 31, "temp_mcu": 35},
    }  # fmt: skip
    n.update(over)
    return n


def make_state(**over: Any) -> dict[str, Any]:
    st: dict[str, Any] = {
        "time_ns": 1, "host_id": 1, "role": 2, "role_name": "MASTER", "term": 3, "believed_master": 1,
        "leader_lost": False, "armed": False, "estop_latched": False, "cycles_sent": 100, "late_cycles": 2,
        "worst_lateness_us": 300,
        "bus": {"rx_frames": 1000, "tx_frames": 500, "tx_failures": 0, "error_frames": 0, "undecoded_frames": 0,
                "bus_off": False, "rx_fps": 600.0},
        "peers": [{"host": h, "seen": h == 2, "role": 1, "term": 3} for h in (1, 2, 3)],
        "nodes": [make_node(4 + i) for i in range(6)],
    }  # fmt: skip
    st.update(over)
    return st


class FakeBridge:
    """Records calls; `state_value` is what state() returns (None = gateway unreachable)."""

    def __init__(self, state: dict[str, Any] | None = None) -> None:
        self.state_value = make_state() if state is None else state
        self.calls: list[tuple[str, Any]] = []
        self.targets: list[list[dict[str, float]]] = []
        self.fail = False

    async def state(self) -> dict[str, Any]:
        if self.fail or self.state_value is None:
            raise BridgeError("down")
        return self.state_value

    async def set_targets(self, wheels: list[dict[str, float]]) -> dict[str, Any]:
        if self.fail:
            raise BridgeError("down")
        self.targets.append(wheels)
        return {"ok": True, "result": "ok"}

    async def _cmd(self, name: str, arg: Any = None) -> dict[str, Any]:
        self.calls.append((name, arg))
        return {"ok": True, "result": "ok"}

    async def arm(self) -> dict[str, Any]:
        return await self._cmd("arm")

    async def disarm(self) -> dict[str, Any]:
        return await self._cmd("disarm")

    async def estop(self) -> dict[str, Any]:
        return await self._cmd("estop")

    async def release_estop(self) -> dict[str, Any]:
        return await self._cmd("release_estop")

    async def request_lead(self, host: int) -> dict[str, Any]:
        return await self._cmd("request_lead", host)

    async def forget_node(self, node_id: int) -> dict[str, Any]:
        return await self._cmd("forget_node", node_id)

    async def close(self) -> None:
        pass


class FakeWS:
    """Just enough of a WebSocket for Hub.serve()."""

    def __init__(self, peer: str = "203.0.113.5") -> None:
        self.client = SimpleNamespace(host=peer)  # like starlette's WebSocket.client
        self.inbox: asyncio.Queue[str | None] = asyncio.Queue()
        self.sent: list[dict[str, Any]] = []
        self.accepted = False

    async def accept(self) -> None:
        self.accepted = True

    async def receive_text(self) -> str:
        item = await self.inbox.get()
        if item is None:
            raise WebSocketDisconnect(1000)
        return item

    async def send_text(self, text: str) -> None:
        self.sent.append(json.loads(text))

    # helpers for tests
    def say(self, **msg: Any) -> None:
        self.inbox.put_nowait(json.dumps(msg))

    def hang_up(self) -> None:
        self.inbox.put_nowait(None)

    def acks(self) -> list[dict[str, Any]]:
        return [m for m in self.sent if m.get("t") == "ack"]
