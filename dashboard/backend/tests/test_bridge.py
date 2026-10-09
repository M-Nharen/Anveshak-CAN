"""GatewayBridge against a scripted Python stand-in for the gateway's socket."""

import asyncio
import json
import os
import tempfile

from anv_dashboard.bridge import BridgeError, GatewayBridge


class FakeGateway:
    def __init__(self, path: str) -> None:
        self.path = path
        self.connections = 0
        self.requests: list[dict] = []
        self.server: asyncio.AbstractServer | None = None
        self.writers: list[asyncio.StreamWriter] = []

    async def start(self) -> None:
        self.server = await asyncio.start_unix_server(self._client, path=self.path)

    async def stop(self) -> None:
        assert self.server is not None
        self.server.close()
        for w in self.writers:  # wait_closed() waits for open connections otherwise
            w.close()
        await self.server.wait_closed()

    async def _client(self, reader, writer) -> None:
        self.connections += 1
        self.writers.append(writer)
        while line := await reader.readline():
            req = json.loads(line)
            self.requests.append(req)
            if req["cmd"] == "state":
                reply = {"id": req["id"], "ok": True, "state": {"host_id": 1}}
            else:
                reply = {"id": req["id"], "ok": True, "code": 0, "result": "ok"}
            writer.write((json.dumps(reply) + "\n").encode())
            await writer.drain()
        writer.close()


def sock_path() -> str:
    return os.path.join(tempfile.mkdtemp(), "gw.sock")


def test_requests_and_replies():
    async def main():
        gw = FakeGateway(sock_path())
        await gw.start()
        b = GatewayBridge(gw.path)
        assert (await b.state())["host_id"] == 1
        assert (await b.arm())["ok"]
        await b.request_lead(2)
        await b.set_targets([{"mode": 2, "a": 1.0, "b": 1.0, "accel": 5.0}] * 6)
        cmds = [r["cmd"] for r in gw.requests]
        assert cmds == ["state", "arm", "request_lead", "set_targets"]
        assert gw.requests[2]["host"] == 2 and len(gw.requests[3]["wheels"]) == 6
        await b.close()
        await gw.stop()

    asyncio.run(main())


def test_estop_uses_its_own_connection():
    async def main():
        gw = FakeGateway(sock_path())
        await gw.start()
        b = GatewayBridge(gw.path)
        await b.state()
        await b.estop()
        assert gw.connections == 2
        await b.estop()
        await b.state()
        assert gw.connections == 2  # connections are reused
        await b.close()
        await gw.stop()

    asyncio.run(main())


def test_unreachable_gateway_raises_and_recovers_when_it_returns():
    async def main():
        path = sock_path()
        b = GatewayBridge(path, timeout=0.5)
        try:
            await b.state()
            raise AssertionError("expected BridgeError")
        except BridgeError:
            pass
        gw = FakeGateway(path)
        await gw.start()
        assert (await b.state())["host_id"] == 1  # reconnects by itself
        await gw.stop()
        try:
            await b.state()  # the connection was closed under us
            raise AssertionError("expected BridgeError")
        except BridgeError:
            pass
        await b.close()

    asyncio.run(main())
