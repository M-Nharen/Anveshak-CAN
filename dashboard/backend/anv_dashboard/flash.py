"""One firmware update at a time, for the screen's Flash button.

Two ways, picked by ANV_DASH_FLASH_MODE:
- "gateway" (v2 bench, the default): GatewayFlash. A .cpp upload is built into an image first
  (firmware_build), then the bench gateway flashes it over CAN and reports progress in its state.
- "anv_flash" (old 11-bit gateway): FlashJobs runs host/tools/anv_flash and follows its JSON."""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from . import firmware_build
from .bridge import BridgeError
from .settings import Settings

Command = Callable[[int, Path], list[str]]


class FlashJobs:
    def __init__(self, settings: Settings, command: Command | None = None) -> None:
        self.settings = settings
        self._command = command or self._anv_flash
        self._status: dict[str, Any] | None = None
        self._proc: asyncio.subprocess.Process | None = None
        self._task: asyncio.Task[None] | None = None

    def _anv_flash(self, node_id: int, path: Path) -> list[str]:
        s = self.settings
        return [sys.executable, "-m", "anv_flash", "--channel", s.can_interface, "--bridge", s.gateway_socket,
                "flash", "--node", str(node_id), "--json", str(path)]  # fmt: skip

    @property
    def busy(self) -> bool:
        return self._proc is not None and self._proc.returncode is None

    def snapshot(self) -> dict[str, Any] | None:
        if self._status is None:
            return None
        return {**self._status, "elapsed_s": round(time.monotonic() - self._status["started"])}

    async def start(self, node_id: int, data: bytes, filename: str) -> str | None:
        """Starts flashing; returns an error message instead if it can't."""
        if self.busy:
            return f"node {self._status['node_id'] if self._status else '?'} is being flashed; wait or cancel it"
        folder = self.settings.firmware_dir
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / f"node{node_id}.bin"
        path.write_bytes(data)
        env = {**os.environ, "PYTHONPATH": os.pathsep.join(filter(None, [str(self.settings.tools_dir), os.environ.get("PYTHONPATH")]))}
        self._status = {"node_id": node_id, "file": Path(filename).name, "phase": "starting", "pct": 0.0, "msg": "",
                        "state": "running", "started": time.monotonic()}  # fmt: skip
        self._proc = await asyncio.create_subprocess_exec(
            *self._command(node_id, path), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT, env=env
        )
        self._task = asyncio.create_task(self._follow(self._proc, path))
        return None

    async def _follow(self, proc: asyncio.subprocess.Process, path: Path) -> None:
        assert proc.stdout is not None and self._status is not None
        status = self._status
        async for raw in proc.stdout:
            line = raw.decode(errors="replace").strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except ValueError:
                status["msg"] = line[-200:]  # e.g. a traceback: keep its last line
                continue
            status.update({k: ev[k] for k in ("phase", "pct", "msg") if k in ev})
        rc = await proc.wait()
        status["state"] = "done" if rc == 0 and status["phase"] == "done" else "failed"
        if rc != 0 and status["phase"] != "error":
            status["msg"] = status["msg"] or f"anv-flash exited with code {rc}"
        with contextlib.suppress(OSError):
            path.unlink()

    def cancel(self) -> bool:
        if not self.busy or self._proc is None:
            return False
        self._proc.terminate()  # anv-flash tells the node to abort; it keeps its current image
        return True

    async def stop(self) -> None:
        self.cancel()
        if self._task is not None:
            with contextlib.suppress(asyncio.CancelledError, TimeoutError):
                await asyncio.wait_for(self._task, timeout=3.0)


class GatewayFlash:
    """Flashing through the v2 bench gateway (host/apps/bench_gateway): the gateway sends the image
    over CAN itself; this side builds it first when the upload is a `user_app.cpp`.

    Phases on the screen: building / using cached build -> write -> verify -> confirm -> done."""

    def __init__(self, settings: Settings, bridge: Any, state: Callable[[], dict[str, Any] | None]) -> None:
        self.settings = settings
        self.bridge = bridge
        self._state = state
        self._status: dict[str, Any] | None = None
        self._task: asyncio.Task[None] | None = None
        self._build_proc: asyncio.subprocess.Process | None = None
        self._cancel = asyncio.Event()

    @property
    def busy(self) -> bool:
        return self._task is not None and not self._task.done()

    def snapshot(self) -> dict[str, Any] | None:
        if self._status is None:
            return None
        return {**self._status, "elapsed_s": round(time.monotonic() - self._status["started"])}

    async def start(self, node_id: int, data: bytes, filename: str) -> str | None:
        if self.busy:
            return f"node {self._status['node_id'] if self._status else '?'} is being flashed; wait or cancel it"
        name = Path(filename).name
        if Path(name).suffix.lower() not in (".bin", ".cpp"):
            return "upload a .bin image or a user_app.cpp"
        self._cancel = asyncio.Event()
        self._status = {"node_id": node_id, "file": name, "phase": "starting", "pct": 0.0, "msg": "",
                        "state": "running", "started": time.monotonic(), "log": []}  # fmt: skip
        self._task = asyncio.create_task(self._run(node_id, data, name))
        return None

    def _set(self, **fields: Any) -> None:
        assert self._status is not None
        self._status.update(fields)

    def _fail(self, msg: str) -> None:
        self._set(phase="error", state="failed", msg=msg)

    def _node(self, node_id: int) -> dict[str, Any] | None:
        st = self._state()
        return next((n for n in (st or {}).get("nodes", []) if n["node_id"] == node_id), None)

    async def _run(self, node_id: int, data: bytes, name: str) -> None:
        try:
            if name.lower().endswith(".cpp"):
                image = await self._image_from_source(node_id, data)
            else:
                folder = self.settings.firmware_dir
                folder.mkdir(parents=True, exist_ok=True)
                image = folder / f"node{node_id}.bin"
                image.write_bytes(data)
            if image is None or self._cancel.is_set():
                if self._cancel.is_set():
                    self._fail("cancelled before flashing; the node keeps its current image")
                return
            await self._flash(node_id, image)
        except firmware_build.BuildError as e:
            self._fail(str(e))
        except BridgeError as e:
            self._fail(f"gateway: {e}")

    async def _image_from_source(self, node_id: int, source: bytes) -> Path | None:
        node = self._node(node_id)
        fqbn = {1: self.settings.fqbn_esp32, 2: self.settings.fqbn_esp32s3}.get((node or {}).get("hw_target", 0))
        if fqbn is None:
            self._fail(f"node {node_id}'s chip isn't known yet (is it on the bus?); can't pick what to build for")
            return None
        repo = self.settings.repo_dir
        key = firmware_build.cache_key(source, fqbn, firmware_build.source_hash(repo))
        cache = self.settings.build_cache_dir
        hit = firmware_build.cached_image(cache, key)
        if hit is not None:
            self._set(phase="cached", msg=f"this file was already built for {node.get('hw_name') or fqbn}")
            return hit
        self._set(phase="build", msg=f"compiling for {node.get('hw_name') or fqbn} (a few minutes on a Pi)")

        def on_line(line: str) -> None:
            assert self._status is not None
            self._status["log"] = (self._status["log"] + [line])[-12:]

        def on_start(proc: asyncio.subprocess.Process) -> None:
            self._build_proc = proc

        try:
            return await firmware_build.build(repo, source, fqbn, cache, key, self.settings.arduino_cli,
                                              on_line, on_start)  # fmt: skip
        finally:
            self._build_proc = None

    async def _flash(self, node_id: int, image: Path) -> None:
        reply = await self.bridge.flash(node_id, str(image))
        if not reply.get("ok"):
            self._fail(str(reply.get("error") or reply.get("result") or "the gateway refused"))
            return
        self._set(phase="write", pct=0.0, msg="")
        while True:  # the gateway reports progress in its state; mirror it
            await asyncio.sleep(0.2)
            fl = (self._state() or {}).get("flash")
            if not fl or fl.get("node_id") != node_id:
                continue
            self._set(phase=fl["phase"], pct=fl["pct"], msg=fl["msg"])
            if fl["state"] != "running":
                self._set(state=fl["state"])
                return

    def cancel(self) -> bool:
        if not self.busy:
            return False
        self._cancel.set()
        if self._build_proc is not None and self._build_proc.returncode is None:
            self._build_proc.terminate()
        else:
            asyncio.ensure_future(self._cancel_flash())
        return True

    async def _cancel_flash(self) -> None:
        with contextlib.suppress(BridgeError):
            await self.bridge.flash_cancel()

    async def stop(self) -> None:
        self.cancel()
        if self._task is not None:
            with contextlib.suppress(asyncio.CancelledError, TimeoutError):
                await asyncio.wait_for(self._task, timeout=3.0)
