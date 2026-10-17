"""One firmware update at a time: run anv-flash for an uploaded image and follow its JSON progress.

The flashing itself lives in host/tools/anv_flash (also used from the command line); this only
starts it, keeps the latest progress for the screen, and stops it on request."""

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
