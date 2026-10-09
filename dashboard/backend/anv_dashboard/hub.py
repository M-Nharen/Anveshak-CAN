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

from anv_can_defs.params_drive import PARAMS_DRIVE, PARAMS_DRIVE_BY_NAME
from starlette.websockets import WebSocketDisconnect

from . import rates as rates_mod
from . import views
from .audit import AuditLog
from .auth import Sessions
from .bridge import Bridge, BridgeError
from .control import ControlManager
from .paramcodec import to_physical, to_raw
from .settings import Settings
from .testbench import JogManager, StepTest

log = logging.getLogger("anv_dashboard")

PAGES = ("overview", "nodes", "drive", "rates", "parameters", "firmware", "test_bench")


def _refusal_reason(reply: dict[str, Any]) -> str:
    status = reply.get("status")
    if status:  # SERVICE STATUS != 0: the node itself refused (e.g. 3 = not allowed while armed)
        return f"node refused: status {status}"
    return str(reply.get("result") or reply.get("error") or "refused")


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

        # Rates / Parameters pages: caches filled by refresh_rates()/refresh_params() (SERVICE
        # GET is too slow to poll every tick, so these are refreshed on page entry and after a
        # change, not continuously). node_rates holds physical Hz, node_params raw wire bits.
        self.node_rates: dict[int, dict[str, float]] = {}
        self.node_params: dict[int, dict[int, int]] = {}
        self.selected_param_node: int | None = None
        self.audit = AuditLog(clock=clock)

        # Test bench
        self.jog = JogManager(settings, clock)
        self.step_test: StepTest | None = None
        self._step_test_holder: str | None = None

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
                self._loop(self.test_bench_once, 1.0 / self.settings.control_hz)
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

    async def test_bench_once(self) -> None:
        """Jog (Test bench page): same 6-wheel bridge call as Drive, with every other drive at
        zero. Not meant to be used at the same time as the Drive page holds control."""
        jogged = self.jog.tick()
        if jogged is None:
            return
        drive, wheel = jogged
        zero = {"mode": 2, "a": 0.0, "b": 0.0, "accel": self.settings.default_accel}
        wheels = [dict(zero) for _ in range(6)]
        wheels[drive - 1] = wheel
        try:
            await self.bridge.set_targets(wheels)
        except BridgeError:
            pass

        if self.step_test is not None and self.state is not None:
            now = self.clock()
            by_id = {n["node_id"]: n for n in self.state["nodes"]}
            node = by_id.get(3 + self.step_test.drive, {})
            fb = node.get("fb")
            target = self.step_test.amplitude if not self.step_test.done(now) else 0.0
            self.step_test.record(now, target, None if fb is None else fb["speed_a"])
            if self.step_test.done(now):
                self.jog.stop(self._step_test_holder)

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
        msg["audit"] = {"recent": self.audit.recent(20), "can_undo": self._can_undo()}
        if self.state is not None:
            if client.page == "overview":
                msg["overview"] = views.overview_view(
                    self.state, self.rates, self.settings
                )
            elif client.page == "nodes":
                msg["nodes"] = views.nodes_view(self.state)
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
            elif client.page == "rates":
                entries = rates_mod.catalog(self.state, self.node_rates)
                msg["rates"] = views.rates_view(entries, rates_mod.BUS_LOAD_GUARD_PCT)
            elif client.page == "parameters":
                params = self.node_params.get(self.selected_param_node, {}) if self.selected_param_node else {}
                msg["parameters"] = views.parameters_view(
                    self.selected_param_node, params, bool(self.state.get("armed"))
                )
            elif client.page == "firmware":
                msg["firmware"] = views.firmware_view(self.state)
            elif client.page == "test_bench":
                msg["test_bench"] = views.test_bench_view(self.state, self.jog.drive, self.step_test)
        return msg

    def _can_undo(self) -> bool:
        return any(e["kind"] in ("rate", "param") for e in self.audit.recent(1))

    # --- Rates page: node rates are cached (SERVICE GET is too slow to poll every tick) ----------
    async def refresh_rates(self) -> None:
        for node_id in rates_mod.DRIVE_NODE_IDS:
            cache = self.node_rates.setdefault(node_id, {})
            for name, _frames, _floor in rates_mod.NODE_RATE_PARAMS:
                info = PARAMS_DRIVE_BY_NAME[name]
                try:
                    reply = await self.bridge.get_param(node_id, info.index)
                except BridgeError:
                    continue
                if reply.get("ok"):
                    cache[name] = to_physical(info.type, reply.get("value_raw", 0))

    async def apply_rate(self, who: str, rate_id: str, new_hz: float) -> tuple[bool, str]:
        entries = rates_mod.catalog(self.state or {}, self.node_rates)
        allowed, reason, _load = rates_mod.check_rate_change(entries, rate_id, new_hz, self.settings, self.state)
        if not allowed:
            self.audit.record("refused", who, target=rate_id, attempted=new_hz, reason=reason)
            return False, reason
        entry = next(e for e in entries if e.id == rate_id)
        old_hz = entry.hz
        if entry.source == "host":
            reply = await self.bridge.set_host_rate(
                cycle_hz=new_hz if rate_id == "cycle" else None,
                host_hb_hz=new_hz if rate_id == "host_hb" else None,
            )
            ok = bool(reply.get("ok"))
        else:
            name = rate_id.split(":", 2)[2]
            info = PARAMS_DRIVE_BY_NAME[name]
            reply = await self.bridge.set_param(entry.node_id, info.index, to_raw(info.type, new_hz))
            ok = bool(reply.get("ok"))
            if ok:
                self.node_rates.setdefault(entry.node_id, {})[name] = new_hz
        if ok:
            self.audit.record("rate", who, target=rate_id, old=old_hz, new=new_hz)
            return True, ""
        reason = _refusal_reason(reply)
        self.audit.record("refused", who, target=rate_id, attempted=new_hz, reason=reason)
        return False, reason

    async def save_rate_defaults(self, who: str) -> tuple[bool, str]:
        failures = []
        for node_id in rates_mod.DRIVE_NODE_IDS:
            for name, _frames, _floor in rates_mod.NODE_RATE_PARAMS:
                info = PARAMS_DRIVE_BY_NAME[name]
                reply = await self.bridge.save_param(node_id, info.index)
                if not reply.get("ok"):
                    failures.append(f"drive {node_id - 3} {name}")
        ok = not failures
        if ok:
            self.audit.record("rate", who, target="save_defaults", old=None, new="saved to flash")
        return ok, "" if ok else "failed to save: " + ", ".join(failures)

    # --- Parameters page ---------------------------------------------------------------------------
    async def refresh_params(self, node_id: int) -> None:
        cache = self.node_params.setdefault(node_id, {})
        for info in PARAMS_DRIVE.values():
            try:
                reply = await self.bridge.get_param(node_id, info.index)
            except BridgeError:
                continue
            if reply.get("ok"):
                cache[info.index] = reply.get("value_raw", 0)

    async def apply_param(self, who: str, node_id: int, index: int, physical: float) -> tuple[bool, str]:
        info = PARAMS_DRIVE.get(index)
        if info is None:
            return False, f"no such parameter index {index}"
        if not (info.minimum <= physical <= info.maximum):
            reason = f"{info.name} must be between {info.minimum:g} and {info.maximum:g}"
            self.audit.record("refused", who, target=info.name, node=node_id, attempted=physical, reason=reason)
            return False, reason
        armed = bool((self.state or {}).get("armed"))
        if info.disarmed_only and armed:
            reason = f"{info.name} can only be changed while disarmed"
            self.audit.record("refused", who, target=info.name, node=node_id, attempted=physical, reason=reason)
            return False, reason
        old_raw = self.node_params.get(node_id, {}).get(index)
        old = None if old_raw is None else to_physical(info.type, old_raw)
        raw = to_raw(info.type, physical)
        reply = await self.bridge.set_param(node_id, index, raw)
        ok = bool(reply.get("ok"))
        if ok:
            self.node_params.setdefault(node_id, {})[index] = raw
            self.audit.record("param", who, target=info.name, node=node_id, old=old, new=physical)
            return True, ""
        reason = _refusal_reason(reply)
        self.audit.record("refused", who, target=info.name, node=node_id, attempted=physical, reason=reason)
        return False, reason

    async def restore_param(self, who: str, node_id: int, index: int) -> tuple[bool, str]:
        info = PARAMS_DRIVE.get(index)
        if info is None:
            return False, f"no such parameter index {index}"
        old_raw = self.node_params.get(node_id, {}).get(index)
        old = None if old_raw is None else to_physical(info.type, old_raw)
        reply = await self.bridge.restore_param(node_id, index)
        ok = bool(reply.get("ok"))
        if not ok:
            reason = _refusal_reason(reply)
            self.audit.record("refused", who, target=info.name, node=node_id, attempted=info.default, reason=reason)
            return False, reason
        self.node_params.setdefault(node_id, {})[index] = to_raw(info.type, info.default)
        self.audit.record("param", who, target=info.name, node=node_id, old=old, new=info.default)
        return True, ""

    # --- Audit and undo ---------------------------------------------------------------------------
    async def undo_last_change(self, who: str) -> tuple[bool, str]:
        entry = self.audit.undo_last()
        if entry is None:
            return False, "nothing to undo"
        if entry["old"] is None:
            return False, "no previous value recorded"
        if entry["kind"] == "rate":
            ok, reason = await self.apply_rate(who, entry["target"], entry["old"])
        else:
            index = PARAMS_DRIVE_BY_NAME[entry["target"]].index
            ok, reason = await self.apply_param(who, entry["node"], index, entry["old"])
        if ok:
            self.audit.record("undo", who, of=entry["id"], target=entry["target"])
        return ok, reason

    # --- one browser ----------------------------------------------------------------------------
    async def serve(self, ws: Any) -> None:
        await ws.accept()
        client = Client(id=f"c{next(self._ids)}", ws=ws)
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
            self.jog.on_disconnect(client.id)

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
                if client.page == "rates":
                    asyncio.create_task(self.refresh_rates())
                elif client.page == "parameters" and self.selected_param_node is not None:
                    asyncio.create_task(self.refresh_params(self.selected_param_node))
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

            # --- Rates page ---------------------------------------------------------------------
            if kind == "set_rate":
                ok, reason = await self.apply_rate(client.id, str(msg.get("rate_id")), float(msg.get("hz")))
                return self._ack(msg, ok, reason)
            if kind == "save_rate_defaults":
                ok, reason = await self.save_rate_defaults(client.id)
                return self._ack(msg, ok, reason)

            # --- Parameters page -----------------------------------------------------------------
            if kind == "select_param_node":
                node_id = msg.get("node")
                self.selected_param_node = int(node_id) if node_id is not None else None
                if self.selected_param_node is not None:
                    asyncio.create_task(self.refresh_params(self.selected_param_node))
                return self._ack(msg, True, "")
            if kind == "set_param":
                node_id, index = int(msg["node"]), int(msg["index"])
                ok, reason = await self.apply_param(client.id, node_id, index, float(msg["value"]))
                return self._ack(msg, ok, reason)
            if kind == "save_param":
                node_id, index = int(msg["node"]), int(msg["index"])
                reply = await self.bridge.save_param(node_id, index)
                return self._bridge_ack(msg, reply)
            if kind == "restore_param":
                node_id, index = int(msg["node"]), int(msg["index"])
                ok, reason = await self.restore_param(client.id, node_id, index)
                return self._ack(msg, ok, reason)

            # --- Audit and undo ------------------------------------------------------------------
            if kind == "undo_last_change":
                ok, reason = await self.undo_last_change(client.id)
                return self._ack(msg, ok, reason)

            # --- Test bench ----------------------------------------------------------------------
            if kind == "jog_start":
                ok = self.jog.start(client.id, int(msg.get("drive", 0)), float(msg.get("speed", 0.0)))
                return self._ack(msg, ok, "" if ok else "bad drive, or another browser is jogging")
            if kind == "jog_stop":
                return self._ack(msg, self.jog.stop(client.id), "")
            if kind == "set_gains":
                drive = int(msg["drive"])
                node_id = 3 + drive
                results = []
                for name, value in (("pid_kp_a", msg.get("kp")), ("pid_ki_a", msg.get("ki")), ("pid_kd_a", msg.get("kd"))):
                    if value is None:
                        continue
                    info = PARAMS_DRIVE_BY_NAME[name]
                    ok, reason = await self.apply_param(client.id, node_id, info.index, float(value))
                    results.append((ok, reason))
                ok = all(r[0] for r in results) if results else False
                reason = "" if ok else next((r[1] for r in results if not r[0]), "no gains given")
                return self._ack(msg, ok, reason)
            if kind == "step_test_start":
                drive = int(msg["drive"])
                amplitude = float(msg.get("amplitude", 1.0))
                duration_s = float(msg.get("duration_s", 2.0))
                if not self.jog.start(client.id, drive, amplitude):
                    return self._ack(msg, False, "bad drive, or another browser is jogging")
                self._step_test_holder = client.id
                self.step_test = StepTest(drive=drive, amplitude=amplitude, duration_s=duration_s, started=self.clock())
                return self._ack(msg, True, "step test started")
        except BridgeError as exc:
            return self._ack(msg, False, str(exc))
        except (TypeError, ValueError, KeyError):
            return self._ack(msg, False, "bad arguments")
        return self._ack(msg, False, f"unknown message '{kind}'")
