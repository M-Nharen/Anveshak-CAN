"""The dashboard's brain: polls the gateway, owns the control lock, answers browsers over WebSocket.

Framework-free on purpose. `Hub.serve()` only needs an object with accept / receive_text /
send_text (a Starlette WebSocket or a test double), so all of this is tested without a web server.
"""

from __future__ import annotations

import asyncio
import contextlib
import itertools
import json
import logging
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from starlette.websockets import WebSocketDisconnect

from . import views
from .auth import Sessions
from .bridge import Bridge, BridgeError
from .control import ControlManager
from .settings import Settings

log = logging.getLogger("anv_dashboard")

PAGES = ("overview", "nodes", "drive", "bench")


@dataclass
class Client:
    id: str
    ws: Any
    authed: bool = False
    page: str = "overview"
    last_push: float = 0.0
    send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)


class Hub:
    def __init__(
        self,
        settings: Settings,
        bridge: Bridge,
        sessions: Sessions | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.settings = settings
        self.bridge = bridge
        self.clock = clock
        self.sessions = sessions or Sessions(
            settings.password, settings.session_ttl_s, clock
        )
        self.control = ControlManager(settings, clock)
        self.rates = views.RateTracker()
        self.state: dict[str, Any] | None = None
        self.clients: dict[str, Client] = {}
        self._ids = itertools.count(1)
        self._tasks: list[asyncio.Task[None]] = []

    # --- lifecycle ------------------------------------------------------------------------------
    def start(self) -> None:
        self._tasks = [
            asyncio.create_task(
                self._loop(self.poll_once, 1.0 / self.settings.poll_hz)
            ),
            asyncio.create_task(
                self._loop(self.control_once, 1.0 / self.settings.control_hz)
            ),
            asyncio.create_task(
                self._loop(
                    self.push_once,
                    1.0 / max(self.settings.push_hz, self.settings.drive_push_hz),
                )
            ),
        ]

    async def stop(self) -> None:
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            with contextlib.suppress(asyncio.CancelledError):
                await t
        self._tasks = []
        await self.bridge.close()

    @staticmethod
    async def _loop(step: Callable[[], Any], period: float) -> None:
        while True:
            started = time.monotonic()
            try:
                await step()
            except asyncio.CancelledError:
                raise
            except (
                Exception
            ):  # keep the loop alive; one bad step must not stop the dashboard
                log.exception("dashboard loop step failed")
            await asyncio.sleep(max(0.0, period - (time.monotonic() - started)))

    # --- periodic steps (also called directly by tests) -----------------------------------------
    async def poll_once(self) -> None:
        try:
            self.state = await self.bridge.state()
        except BridgeError:
            self.state = None
            return
        bus = self.state["bus"]
        self.rates.update(
            self.clock(),
            {"tx_frames": bus["tx_frames"], "error_frames": bus["error_frames"]},
        )

    async def control_once(self) -> None:
        wheels = self.control.tick()
        if wheels is None:
            return
        try:
            await self.bridge.set_targets(wheels)
        except BridgeError:
            pass  # the gateway zeroes stale targets itself within 100 ms

    async def push_once(self) -> None:
        now = self.clock()
        for client in list(self.clients.values()):
            rate = (
                self.settings.drive_push_hz
                if client.page == "drive"
                else self.settings.push_hz
            )
            if now - client.last_push < 1.0 / rate - 1e-3:
                continue
            client.last_push = now
            await self._send(client, self.snapshot(client))

    def snapshot(self, client: Client) -> dict[str, Any]:
        holder = self.control.holder
        control = {
            "source": "dashboard" if holder else "none",
            "mine": holder == client.id,
            "taken": holder is not None,
        }
        msg: dict[str, Any] = {
            "t": "state",
            "page": client.page,
            "authed": client.authed,
        }
        msg["header"] = views.header_view(self.state, control)
        if self.state is not None:
            if client.page == "overview":
                msg["overview"] = views.overview_view(
                    self.state, self.rates, self.settings
                )
            elif client.page == "nodes":
                msg["nodes"] = views.nodes_view(self.state)
            elif client.page == "bench":
                msg["bench"] = views.bench_view(self.state)
            elif client.page == "drive":
                lim = self.control.limits
                msg["drive"] = {
                    "wheels": views.drive_view(self.state, self.control.last_targets),
                    "limits": {
                        "cap": lim.cap,
                        "accel": lim.accel,
                        "level": lim.level,
                        "max_cap": self.settings.max_wheel_speed,
                        "max_accel": self.settings.max_accel,
                    },
                }
        return msg

    # --- one browser ----------------------------------------------------------------------------
    async def serve(self, ws: Any) -> None:
        await ws.accept()
        client = Client(id=f"c{next(self._ids)}", ws=ws)
        # The RPi's own touch screen: whoever can touch it is standing at the rover, so (only when
        # configured) a browser on this machine is treated as logged in.
        peer = getattr(getattr(ws, "client", None), "host", None)
        if self.settings.trust_localhost and peer in ("127.0.0.1", "::1"):
            client.authed = True
        self.clients[client.id] = client
        try:
            while True:
                text = await ws.receive_text()
                try:
                    msg = json.loads(text)
                except ValueError:
                    await self._send(
                        client, {"t": "ack", "ok": False, "msg": "bad json"}
                    )
                    continue
                if not isinstance(msg, dict):
                    continue
                ack = await self.handle(client, msg)
                if ack is not None:
                    await self._send(client, ack)
        except WebSocketDisconnect:
            pass
        finally:
            self.clients.pop(client.id, None)
            self.control.on_disconnect(client.id)  # the lock goes with the browser

    async def _send(self, client: Client, msg: dict[str, Any]) -> None:
        try:
            async with client.send_lock:
                await client.ws.send_text(json.dumps(msg))
        except (RuntimeError, OSError, WebSocketDisconnect):
            log.debug(
                "send to %s failed; serve() cleans up", client.id
            )  # the socket is gone

    @staticmethod
    def _ack(msg: dict[str, Any], ok: bool, text: str = "") -> dict[str, Any]:
        return {"t": "ack", "req": msg.get("req"), "ok": ok, "msg": text}

    def _bridge_ack(self, msg: dict[str, Any], reply: dict[str, Any]) -> dict[str, Any]:
        return self._ack(
            msg,
            bool(reply.get("ok")),
            str(reply.get("result") or reply.get("error") or ""),
        )

    async def handle(
        self, client: Client, msg: dict[str, Any]
    ) -> dict[str, Any] | None:
        kind = msg.get("t")
        if kind == "auth":
            client.authed = self.sessions.valid(msg.get("token"))
            return self._ack(
                msg, client.authed, "" if client.authed else "login failed"
            )
        if kind == "page":
            if msg.get("page") in PAGES:
                client.page = msg["page"]
                client.last_push = 0.0
                return None
            return self._ack(msg, False, "unknown page")
        try:
            if (
                kind == "estop"
            ):  # always allowed, straight to the gateway, ahead of everything else
                self.control.halt()
                return self._bridge_ack(msg, await self.bridge.estop())

            if not client.authed:
                return self._ack(msg, False, "login required")

            if kind == "release_estop":
                return self._bridge_ack(msg, await self.bridge.release_estop())
            if kind == "arm":
                return self._bridge_ack(msg, await self.bridge.arm())
            if kind == "disarm":
                return self._bridge_ack(msg, await self.bridge.disarm())
            if kind == "request_lead":
                host = msg.get("host")
                if host not in (1, 2):
                    return self._ack(msg, False, "host must be 1 or 2")
                return self._bridge_ack(msg, await self.bridge.request_lead(host))
            if kind == "forget_node":
                node_id = msg.get("node_id")
                if not isinstance(node_id, int) or not 1 <= node_id <= 31:
                    return self._ack(msg, False, "node_id must be 1-31")
                return self._bridge_ack(msg, await self.bridge.forget_node(node_id))
            if kind == "take_control":
                if self.control.take(client.id):
                    return self._ack(msg, True, "you have manual control")
                return self._ack(msg, False, "another browser has manual control")
            if kind == "release_control":
                return self._ack(msg, self.control.release(client.id), "")
            if kind == "drive":  # 20 Hz while held: only failures are answered
                ok = self.control.drive(
                    client.id, msg.get("fwd", 0.0), msg.get("turn", 0.0)
                )
                return (
                    None
                    if ok
                    else self._ack(msg, False, "you do not hold manual control")
                )
            if kind == "speed_step":
                ok = self.control.step_speed(
                    client.id, 1 if msg.get("dir", 1) > 0 else -1
                )
                return self._ack(
                    msg, ok, "" if ok else "you do not hold manual control"
                )
            if kind == "limits":
                ok = self.control.set_limits(
                    client.id, msg.get("cap"), msg.get("accel")
                )
                return self._ack(
                    msg, ok, "" if ok else "you do not hold manual control"
                )
        except BridgeError as exc:
            return self._ack(msg, False, str(exc))
        except (TypeError, ValueError):
            return self._ack(msg, False, "bad arguments")
        return self._ack(msg, False, f"unknown message '{kind}'")
