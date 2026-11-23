from __future__ import annotations

import io
import os
import shutil
import subprocess
import threading

import pytest
from conftest import FakeNode, eventually

import anv_can
from anv_can import host_cli


@pytest.fixture(scope="module")
def schema():
    return anv_can.load_schema()


def run(bus, *argv: str) -> tuple[int, str]:
    out = io.StringIO()
    code = host_cli.main(["--wait", "1", *argv], lambda _name: bus.connect(), out)
    return code, out.getvalue()


def test_list_get_set_rate(schema):
    bus = anv_can.TestBus()
    n = FakeNode(bus, schema, "motor_controller", 21)
    b = FakeNode(bus, schema, "esp32_bench", 20)
    code, out = run(bus, "list", "x")
    assert code == 0
    lines = out.splitlines()
    assert "TYPE" in lines[0]
    assert any(" 20  esp32_bench" in ln and "idle" in ln and " ok " in ln for ln in lines)
    assert any(" 21  motor_controller" in ln for ln in lines)

    code, out = run(bus, "get", "x", "21", "kp_0", "heartbeat_hz")
    assert code == 0 and "kp_0 = 10" in out and "heartbeat_hz = 10 Hz" in out
    code, out = run(bus, "get", "x", "21")  # every setting
    assert code == 0 and "max_speed = 50 rad/s" in out and "led_mode = " in out
    code, out = run(bus, "set", "x", "21", "kp_0=12.5", "max_speed=20", "--save")
    assert code == 0 and "kp_0 = 12.5" in out and "saved" in out and n.saves == 1
    code, _ = run(bus, "set", "x", "21", "kp_0=99999")
    assert code == 1
    code, out = run(bus, "rate", "x", "21", "feedback", "10")
    assert code == 0 and "feedback = 10 Hz" in out
    assert n.rates[schema.type("motor_controller").message("feedback").port] == 10000
    code, out = run(bus, "rate", "x", "21")
    assert "feedback = 10 Hz" in out and "thermal = 1 Hz" in out
    code, out = run(bus, "action", "x", "20", "blink", "2")
    assert code == 0 and b.actions
    code, _ = run(bus, "get", "x", "99")
    assert code == 1
    n.stop()
    b.stop()


def test_arm_needs_lead_and_estop(schema):
    bus = anv_can.TestBus()
    n = FakeNode(bus, schema, "motor_controller", 21)
    code, _ = run(bus, "arm", "x", "21", "--hold", "0.2")
    assert code == 1 and not n.armed  # no --take-lead
    code, out = run(bus, "command", "x", "21", "setpoint", "mode=velocity", "target=[1,1]",
                    "--take-lead", "--arm", "--duration", "0.5")
    assert code == 0 and "sending setpoint" in out
    assert n.commands >= 10 and n.leader == 250
    assert eventually(lambda: not n.armed)  # the tool stopped: disarmed
    code, out = run(bus, "estop", "x", "--reason", "4")
    assert code == 0 and eventually(lambda: n.estop)
    code, _ = run(bus, "estop", "x", "--release", "--take-lead")
    assert code == 0 and eventually(lambda: not n.estop)
    n.stop()


def test_debug_level(schema):
    bus = anv_can.TestBus()
    n = FakeNode(bus, schema, "motor_controller", 21)
    code, out = run(bus, "debug", "x", "21", "2", "--take-lead", "--timeout", "30")
    assert code == 0 and "debug level 2" in out and "for 30" in out
    assert eventually(lambda: n.debug_level == 2 and n.debug_sets == 1)
    n.stop()


def test_events(schema):
    bus = anv_can.TestBus()
    n = FakeNode(bus, schema, "motor_controller", 21)
    threading.Timer(0.1, n.send_event, kwargs={"code": 0x0101, "detail": 7, "severity": 1}).start()
    code, out = run(bus, "events", "x", "--duration", "0.5")
    assert code == 0 and "watching for events" in out
    assert "node  21  warning  can_error_passive  detail=7" in out
    n.stop()


def _vcan_up() -> bool:
    return os.path.exists("/sys/class/net/vcan0")


@pytest.mark.skipif(not _vcan_up() or shutil.which("anv-host") is None, reason="needs vcan0")
def test_vcan_cli(schema):
    """The installed anv-host on a real SocketCAN interface, against a fake node on vcan0."""
    n = FakeNode(anv_can.open_socketcan("vcan0"), schema, "motor_controller", 21)
    try:
        def cli(*args: str) -> str:
            r = subprocess.run(["anv-host", *args], capture_output=True, text=True, timeout=20)
            assert r.returncode == 0, r.stderr
            return r.stdout

        assert "motor_controller" in cli("list", "vcan0")
        assert "kp_0 = 12.5" in cli("set", "vcan0", "21", "kp_0=12.5")
        assert "kp_0 = 12.5" in cli("get", "vcan0", "21", "kp_0")
        assert "feedback = 20 Hz" in cli("rate", "vcan0", "21", "feedback", "20")
        assert "armed 21" in cli("arm", "vcan0", "21", "--take-lead", "--hold", "0.05")
        cli("estop", "vcan0")
        assert eventually(lambda: n.estop)
    finally:
        n.stop()


def test_id_service_and_set_id(schema, tmp_path, monkeypatch):
    """The Pi's ID service hands boards without switches IDs from 16 up, keeps them in its table,
    and `set-id` moves one; a DIP/fixed ID can't be moved."""
    table = tmp_path / "node_ids.txt"
    monkeypatch.setenv("ANV_ID_TABLE", str(table))
    bus = anv_can.TestBus()
    serve = threading.Thread(target=run, args=(bus, "ids", "x", "--serve"),
                             daemon=True)
    serve.start()
    a = FakeNode(bus, schema, "esp32_bench", 0, mac=0xA4CF12000001)
    b = FakeNode(bus, schema, "esp32_bench", 0, mac=0xA4CF12000002)
    dip = FakeNode(bus, schema, "esp32_bench", 3, mac=0xA4CF12000003)
    lo = schema.node_ids["auto_first"]
    assert lo == 16
    assert eventually(lambda: {a.id, b.id} == {lo, lo + 1}, 3.0)
    assert eventually(lambda: "a4cf12000001" in table.read_text() if table.exists() else False)

    code, out = run(bus, "ids", "x")
    assert code == 0 and "a4:cf:12:00:00:01" in out and "esp32_bench" in out and "yes" in out

    code, out = run(bus, "set-id", "x", str(a.id), "5")
    assert code == 0, out
    assert a.id == 5 and "now node 5" in out
    assert "a4cf12000001 5 " in table.read_text()
    code, out = run(bus, "set-id", "x", "3", "7")  # the DIP board
    assert code == 1 and "can't be moved" in out and dip.id == 3
    code, out = run(bus, "set-id", "x", "5", "3")  # taken
    assert code == 1 and "already in use" in out

    # A board that lost its ID (forget_id, fresh flash) gets the one in the table back.
    a.id = 0
    assert eventually(lambda: a.id == 5, 3.0)
    for n in (a, b, dip):
        n.stop()
