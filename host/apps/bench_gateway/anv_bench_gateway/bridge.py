"""The dashboard's socket: one JSON object per line, request -> one reply line.

The same protocol as the old 11-bit gateway's bridge (host/gateway/src/bridge.cpp), so the
dashboard backend talks to this daemon unchanged:

    {"id":1,"cmd":"state"}                          -> {"id":1,"ok":true,"state":{...}}
    {"id":2,"cmd":"arm"} | "disarm" | "estop" | "release_estop"
    {"id":3,"cmd":"request_lead","host":2}
    {"id":4,"cmd":"send_text","node_id":16,"text":"hello"}   (node 0 = everyone; reply has "seq")
    {"id":5,"cmd":"forget_node","node_id":17}
    {"id":6,"cmd":"flash","node_id":16,"path":"/tmp/x.bin"}  (progress in state.flash)
    {"id":7,"cmd":"flash_cancel"}
"""

from __future__ import annotations

import json
import os
import socket
import threading
from typing import Any

import anv_can

from .gateway import BenchGateway

MAX_LINE = 64 * 1024


class Bridge:
    def __init__(self, gateway: BenchGateway, path: str) -> None:
        self.gw = gateway
        self.path = path
        self._sock: socket.socket | None = None
        self._running = False
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        if os.path.exists(self.path):
            os.unlink(self.path)  # a stale socket from a previous run
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.bind(self.path)
        os.chmod(self.path, 0o660)
        self._sock.listen(8)
        self._sock.settimeout(0.2)
        self._running = True
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=2)
        if self._sock:
            self._sock.close()
        if os.path.exists(self.path):
            os.unlink(self.path)

    def _accept_loop(self) -> None:
        assert self._sock is not None
        while self._running:
            try:
                conn, _ = self._sock.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn: socket.socket) -> None:
        conn.settimeout(1.0)
        buf = b""
        with conn:
            while self._running:
                try:
                    chunk = conn.recv(4096)
                except TimeoutError:
                    continue
                except OSError:
                    return
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    try:
                        conn.sendall((json.dumps(self.handle(line.decode(errors="replace"))) + "\n").encode())
                    except OSError:
                        return
                if len(buf) > MAX_LINE:
                    return

    def handle(self, line: str) -> dict[str, Any]:
        try:
            req = json.loads(line)
        except ValueError:
            return {"id": None, "ok": False, "error": "bad json"}
        if not isinstance(req, dict):
            return {"id": None, "ok": False, "error": "bad json"}
        rid = req.get("id")
        cmd = req.get("cmd")

        def reply(ok: bool, result: str = "", **extra: Any) -> dict[str, Any]:
            return {
                "id": rid,
                "ok": ok,
                "result": result if ok else "",
                "error": "" if ok else result,
                **extra,
            }

        def host_call(fn, *args: Any) -> dict[str, Any]:
            try:
                fn(*args)
                return reply(True, "ok")
            except anv_can.HostError as e:
                return reply(False, str(e))

        g = self.gw
        if cmd == "ping":
            return reply(True, "pong")
        if cmd == "state":
            return {"id": rid, "ok": True, "state": g.state()}
        if cmd == "arm":
            return host_call(g.host.arm, 0)
        if cmd == "disarm":
            return host_call(g.host.disarm, 0)
        if cmd == "estop":
            return host_call(g.host.estop)
        if cmd == "release_estop":
            return host_call(g.host.estop_release)
        if cmd == "request_lead":
            host = req.get("host")
            if not isinstance(host, int):
                return reply(False, "host must be a host ID")
            return host_call(g.host.request_lead, host)
        if cmd == "send_text":
            node, text = req.get("node_id"), req.get("text")
            if not isinstance(node, int) or not isinstance(text, str):
                return reply(False, "send_text needs node_id and text")
            ok, msg, seq = g.send_text(node, text)
            return reply(ok, msg, seq=seq) if ok else reply(False, msg)
        if cmd == "forget_node":
            node = req.get("node_id")
            if not isinstance(node, int):
                return reply(False, "node_id must be a number")
            return reply(*g.forget_node(node))
        if cmd == "flash":
            node, path = req.get("node_id"), req.get("path")
            if not isinstance(node, int) or not isinstance(path, str):
                return reply(False, "flash needs node_id and path")
            return reply(*g.flash(node, path))
        if cmd == "flash_cancel":
            ok = g.flash_cancel()
            return reply(
                ok, "cancelling; the node keeps its current image" if ok else "nothing is being flashed"
            )
        if cmd == "set_targets":
            return reply(False, "bench nodes take no drive targets")
        return reply(False, f"unknown cmd {cmd!r}")
