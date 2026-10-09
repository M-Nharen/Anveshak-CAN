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
