"""Client for the gateway's local bridge socket (JSON lines, see host/gateway/include/anv_gateway/bridge.hpp).

Two connections: a normal one, and one kept only for E-stop so it never waits behind other requests.
"""

from __future__ import annotations

import asyncio
import json
from typing import Any, Protocol


class BridgeError(Exception):
    """The gateway cannot be reached or answered nonsense."""


class Bridge(Protocol):
    async def state(self) -> dict[str, Any]: ...
    async def set_targets(self, wheels: list[dict[str, float]]) -> dict[str, Any]: ...
    async def arm(self) -> dict[str, Any]: ...
    async def disarm(self) -> dict[str, Any]: ...
    async def estop(self) -> dict[str, Any]: ...
    async def release_estop(self) -> dict[str, Any]: ...
    async def request_lead(self, host: int) -> dict[str, Any]: ...
    async def get_param(self, node: int, index: int) -> dict[str, Any]: ...
    async def set_param(self, node: int, index: int, value_raw: int) -> dict[str, Any]: ...
    async def save_param(self, node: int, index: int) -> dict[str, Any]: ...
    async def restore_param(self, node: int, index: int) -> dict[str, Any]: ...
    async def set_host_rate(
        self, cycle_hz: float | None = None, host_hb_hz: float | None = None
    ) -> dict[str, Any]: ...
    async def close(self) -> None: ...


class _Line:
    """One connection: serialised request/response, reconnecting on the next call after a failure."""

    def __init__(self, path: str, timeout: float) -> None:
        self._path = path
        self._timeout = timeout
        self._lock = asyncio.Lock()
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._next_id = 0

    async def call(self, cmd: str, **fields: Any) -> dict[str, Any]:
        async with self._lock:
            try:
                if self._writer is None:
                    self._reader, self._writer = await asyncio.wait_for(
                        asyncio.open_unix_connection(self._path), self._timeout
                    )
                assert self._reader is not None and self._writer is not None
                self._next_id += 1
                req = {"id": self._next_id, "cmd": cmd, **fields}
                self._writer.write((json.dumps(req) + "\n").encode())
                await asyncio.wait_for(self._writer.drain(), self._timeout)
                line = await asyncio.wait_for(self._reader.readline(), self._timeout)
                if not line:
                    raise ConnectionError("gateway closed the connection")
                reply = json.loads(line)
            except (OSError, ConnectionError, asyncio.TimeoutError, ValueError) as exc:
                await self._drop()
                raise BridgeError(f"gateway unreachable: {exc}") from exc
            if not isinstance(reply, dict):
                await self._drop()
                raise BridgeError("gateway sent a non-object reply")
            return reply

    async def _drop(self) -> None:
        if self._writer is not None:
            self._writer.close()
        self._reader = self._writer = None

    async def close(self) -> None:
        async with self._lock:
            await self._drop()


class GatewayBridge:
    def __init__(self, path: str, timeout: float = 1.0) -> None:
        self._main = _Line(path, timeout)
        self._urgent = _Line(path, timeout)

    async def state(self) -> dict[str, Any]:
        reply = await self._main.call("state")
        state = reply.get("state")
        if not reply.get("ok") or not isinstance(state, dict):
            raise BridgeError(f"bad state reply: {reply.get('error', reply)}")
        return state

    async def set_targets(self, wheels: list[dict[str, float]]) -> dict[str, Any]:
        return await self._main.call("set_targets", wheels=wheels)

    async def arm(self) -> dict[str, Any]:
        return await self._main.call("arm")

    async def disarm(self) -> dict[str, Any]:
        return await self._main.call("disarm")

    async def estop(self) -> dict[str, Any]:
        return await self._urgent.call(
            "estop"
        )  # own connection: never queued behind a state poll

    async def release_estop(self) -> dict[str, Any]:
        return await self._main.call("release_estop")

    async def request_lead(self, host: int) -> dict[str, Any]:
        return await self._main.call("request_lead", host=host)

    async def get_param(self, node: int, index: int) -> dict[str, Any]:
        return await self._main.call("get_param", node=node, index=index)

    async def set_param(self, node: int, index: int, value_raw: int) -> dict[str, Any]:
        return await self._main.call("set_param", node=node, index=index, value_raw=value_raw)

    async def save_param(self, node: int, index: int) -> dict[str, Any]:
        return await self._main.call("save_param", node=node, index=index)

    async def restore_param(self, node: int, index: int) -> dict[str, Any]:
        return await self._main.call("restore_param", node=node, index=index)

    async def set_host_rate(
        self, cycle_hz: float | None = None, host_hb_hz: float | None = None
    ) -> dict[str, Any]:
        fields: dict[str, Any] = {}
        if cycle_hz is not None:
            fields["cycle_hz"] = cycle_hz
        if host_hb_hz is not None:
            fields["host_hb_hz"] = host_hb_hz
        return await self._main.call("set_host_rate", **fields)

    async def close(self) -> None:
        await self._main.close()
        await self._urgent.close()
