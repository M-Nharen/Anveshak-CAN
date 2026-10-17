"""FastAPI wiring: login, health, the WebSocket, and the built frontend as static files."""

from __future__ import annotations

import logging
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request, WebSocket
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from .bridge import Bridge, GatewayBridge
from .hub import Hub
from .settings import Settings

log = logging.getLogger("anv_dashboard")


def create_app(
    settings: Settings | None = None, bridge: Bridge | None = None
) -> FastAPI:
    settings = settings or Settings.from_env()
    hub = Hub(settings, bridge or GatewayBridge(settings.gateway_socket))
    if hub.sessions.generated:
        # No password configured: print the random one so the operator can log in.
        print(
            f"dashboard: no ANV_DASH_PASSWORD set, using generated password: {hub.sessions.password}",
            flush=True,
        )

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncIterator[None]:
        hub.start()
        yield
        await hub.stop()

    app = FastAPI(title="Anveshak dashboard", lifespan=lifespan)
    app.state.hub = hub

    @app.get("/api/health")
    async def health() -> dict[str, object]:
        return {
            "ok": True,
            "gateway": hub.state is not None,
            "clients": len(hub.clients),
        }

    @app.post("/api/login")
    async def login(request: Request) -> JSONResponse:
        try:
            body = await request.json()
            password = str(body.get("password", ""))
        except (ValueError, AttributeError):
            return JSONResponse({"ok": False, "error": "bad request"}, status_code=400)
        token = hub.sessions.login(password)
        if token is None:
            return JSONResponse(
                {"ok": False, "error": "wrong password"}, status_code=401
            )
        return JSONResponse({"ok": True, "token": token})

    @app.post("/api/flash/{node_id}")
    async def flash(node_id: int, request: Request) -> JSONResponse:
        """Body = the app image (.ino.bin), header X-Filename = its name. Needs the login token, or
        a request from this machine when ANV_DASH_TRUST_LOCALHOST is on (the RPi's own screen)."""
        token = request.headers.get("authorization", "").removeprefix("Bearer ").strip()
        local = request.client is not None and request.client.host in ("127.0.0.1", "::1")
        if not (hub.sessions.valid(token) or (settings.trust_localhost and local)):
            return JSONResponse({"ok": False, "error": "login required"}, status_code=401)
        if not 4 <= node_id <= 30:
            return JSONResponse({"ok": False, "error": "node must be 4-30"}, status_code=400)
        data = await request.body()
        if not data:
            return JSONResponse({"ok": False, "error": "no file"}, status_code=400)
        if len(data) > settings.max_firmware_bytes:
            return JSONResponse({"ok": False, "error": "file too large for a firmware image"}, status_code=413)
        error = await hub.flash.start(node_id, data, request.headers.get("x-filename", "firmware.bin"))
        if error:
            return JSONResponse({"ok": False, "error": error}, status_code=409)
        return JSONResponse({"ok": True})

    @app.websocket("/ws")
    async def ws_endpoint(websocket: WebSocket) -> None:
        await hub.serve(websocket)

    if settings.static_dir and settings.static_dir.is_dir():
        # Last, so /api and /ws win.
        app.mount(
            "/",
            StaticFiles(directory=str(settings.static_dir), html=True),
            name="frontend",
        )
    return app
