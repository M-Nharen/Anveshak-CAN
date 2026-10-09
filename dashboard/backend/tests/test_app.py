"""The FastAPI layer end to end: HTTP login, the WebSocket and static files (needs fastapi + httpx)."""

import json
import time

from fastapi.testclient import TestClient

from anv_dashboard.app import create_app
from anv_dashboard.settings import Settings
from fakes import FakeBridge


def make_client(**over):
    settings = Settings(password="pw", poll_hz=50.0, **over)
    bridge = FakeBridge()
    return TestClient(create_app(settings, bridge)), bridge


def login(client) -> str:
    r = client.post("/api/login", json={"password": "pw"})
    assert r.status_code == 200
    return r.json()["token"]


def send(ws, **msg):
    ws.send_text(json.dumps(msg))


def next_of(ws, kind: str, limit: int = 100):
    for _ in range(limit):
        msg = ws.receive_json()
        if msg["t"] == kind:
            return msg
    raise AssertionError(f"no {kind} message")


def test_health_and_login():
    with make_client()[0] as c:
        assert c.get("/api/health").json()["ok"] is True
        assert c.post("/api/login", json={"password": "wrong"}).status_code == 401
        assert c.post("/api/login", content=b"nonsense").status_code == 400
        assert login(c)


def test_websocket_pushes_state_and_enforces_login():
    client, bridge = make_client()
    with client as c, c.websocket_connect("/ws") as ws:
        state = next_of(ws, "state")
        assert state["header"]["leader"] == 1 and "overview" in state
        send(ws, t="arm", req=1)
        assert next_of(ws, "ack")["ok"] is False  # not logged in
        send(ws, t="auth", token=login(c), req=2)
        assert next_of(ws, "ack")["ok"] is True
        send(ws, t="arm", req=3)
        assert next_of(ws, "ack")["ok"] is True
        assert ("arm", None) in bridge.calls


def test_second_browser_cannot_take_control_over_a_real_websocket():
    client, _ = make_client()
    with client as c:
        token = login(c)
        with c.websocket_connect("/ws") as a, c.websocket_connect("/ws") as b:
            for ws in (a, b):
                send(ws, t="auth", token=token, req=0)
                next_of(ws, "ack")
            send(a, t="take_control", req=1)
            assert next_of(a, "ack")["ok"] is True
            send(b, t="take_control", req=1)
            assert next_of(b, "ack")["ok"] is False
            a.close()
            deadline = time.time() + 3
            ok = False
            while (
                not ok and time.time() < deadline
            ):  # the lock frees when the server notices the close
                send(b, t="take_control", req=2)
                ok = next_of(b, "ack")["ok"]
                if not ok:
                    time.sleep(0.05)
            assert ok


def test_drive_messages_become_targets_and_stop_by_themselves():
    client, bridge = make_client(hold_timeout_s=0.2)
    with client as c, c.websocket_connect("/ws") as ws:
        send(ws, t="auth", token=login(c), req=0)
        next_of(ws, "ack")
        send(ws, t="take_control", req=1)
        next_of(ws, "ack")
        send(ws, t="drive", fwd=1.0, turn=0.0)
        time.sleep(0.1)
        assert any(w[0]["a"] > 0 for w in bridge.targets)
        time.sleep(0.5)  # nothing more from the browser
        assert bridge.targets[-1][0]["a"] == 0.0


def test_rates_and_parameters_pages_serialize_over_a_real_websocket():
    client, _ = make_client()
    with client as c, c.websocket_connect("/ws") as ws:
        next_of(ws, "state")
        send(ws, t="auth", token=login(c), req=0)
        next_of(ws, "ack")

        send(ws, t="page", page="rates")
        state = next_of(ws, "state")
        assert state["rates"]["guard_pct"] == 50.0
        assert any(e["id"] == "cycle" for e in state["rates"]["entries"])

        send(ws, t="select_param_node", node=4, req=1)
        assert next_of(ws, "ack")["ok"] is True
        send(ws, t="page", page="parameters")
        state = next_of(ws, "state")
        assert state["parameters"]["node_id"] == 4
        assert len(state["parameters"]["params"]) == 14

        send(ws, t="page", page="firmware")
        state = next_of(ws, "state")
        assert len(state["firmware"]) == 6  # the fake state's six drive nodes

        send(ws, t="page", page="test_bench")
        state = next_of(ws, "state")
        assert len(state["test_bench"]["drives"]) == 6


def test_static_frontend_is_served_after_the_api(tmp_path):
    (tmp_path / "index.html").write_text("<h1>dash</h1>")
    with make_client(static_dir=tmp_path)[0] as c:
        assert "dash" in c.get("/").text
        assert c.get("/api/health").json()["ok"] is True
