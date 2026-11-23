from __future__ import annotations

import pytest
from conftest import FakeNode, eventually

import anv_can


@pytest.fixture(scope="module")
def schema():
    return anv_can.load_schema()


@pytest.fixture
def bus():
    return anv_can.TestBus()


def make_host(bus, schema, host_id: int = 1, **kw) -> anv_can.Host:
    kw.setdefault("allocator", "off")
    kw.setdefault("node_timeout_ms", 300)
    kw.setdefault("service_timeout_ms", 200)
    return anv_can.Host(bus.connect(), schema, host_id=host_id, **kw)


def test_discovery_and_callbacks(bus, schema):
    h = make_host(bus, schema)
    seen, lost = [], []
    h.set_callbacks(node_appeared=lambda n, returned: seen.append((n.id, n.compat)),
                    node_lost=lambda n: lost.append(n.id))
    h.start()
    a = FakeNode(bus, schema, "esp32_bench", 20)
    b = FakeNode(bus, schema, "motor_controller", 21)
    assert h.wait_for_node(20, 1000) and h.wait_for_node(21, 1000)
    assert eventually(lambda: len(seen) == 2)
    assert {n.id: n.type.name for n in h.nodes()} == {20: "esp32_bench", 21: "motor_controller"}
    assert h.node(21).usable and h.node(21).compat == "ok"
    assert anv_can.enum_label(schema, "node_state", h.node(20).state) == "idle"
    a.stop()
    assert eventually(lambda: lost == [20])
    assert not h.node(20).online
    with pytest.raises(anv_can.HostError, match="no_such_node"):
        h.get(20, "heartbeat_hz")
    b.stop()
    h.stop()


def test_callbacks_only_before_start(bus, schema):
    h = make_host(bus, schema)
    with pytest.raises(TypeError, match="unknown callback"):
        h.set_callbacks(nope=print)
    h.start()
    with pytest.raises(anv_can.HostError):
        h.set_callbacks(node_lost=print)
    h.stop()


def test_settings_rates_actions(bus, schema):
    h = make_host(bus, schema)
    watcher = make_host(bus, schema, host_id=2, observer=True)
    changes, telemetry = [], []
    watcher.set_callbacks(setting_changed=lambda node, name, v: changes.append((node, name, v)),
                          telemetry=lambda node, msg, values, ts: telemetry.append(
                              (node, msg, values)))
    h.start()
    watcher.start()
    n = FakeNode(bus, schema, "motor_controller", 21)
    assert h.wait_for_node(21, 1000)
    assert h.get(21, "kp_0") == schema.type("motor_controller").param("kp_0").default
    assert h.set(21, "kp_0", 12.5) == 12.5
    assert h.get(21, "kp_0") == 12.5
    assert eventually(lambda: (21, "kp_0", 12.5) in changes)
    with pytest.raises(anv_can.HostError, match="out_of_range"):
        h.set(21, "kp_0", 1e6)
    h.save(21)
    assert n.saves == 1
    assert h.set_rate(21, "feedback", 5) == 5
    assert h.get_rate(21, "feedback") == 5
    assert eventually(lambda: any(m == "feedback" and "speed" in v for _, m, v in telemetry))
    n.stop()
    b = FakeNode(bus, schema, "esp32_bench", 20)
    assert h.wait_for_node(20, 1000)
    h.action(20, "blink", 3)
    assert b.actions == [(schema.type("esp32_bench").actions[0].index, 3)]
    b.stop()
    h.stop()
    watcher.stop()


def test_lead_arm_command_estop(bus, schema):
    a = make_host(bus, schema, host_id=1)
    b = make_host(bus, schema, host_id=2)
    roles = []
    a.set_callbacks(role_changed=lambda role, term: roles.append(role))
    a.start()
    b.start()
    n = FakeNode(bus, schema, "motor_controller", 21)
    assert a.wait_for_node(21, 1000)
    with pytest.raises(anv_can.HostError, match="not_leader"):
        a.arm(21)
    a.request_lead()
    assert a.leading and a.role == "leader" and eventually(lambda: b.leader == 1 and n.leader == 1)
    a.arm(21)
    a.set_command(21, "setpoint", {"mode": "velocity", "target": [1.0, 1.0]}, 50)
    assert eventually(lambda: n.armed)
    assert eventually(lambda: n.commands > 10)
    assert n.armed
    b.estop(3)
    assert eventually(lambda: n.estop and not n.armed and a.estop_latched)
    a.estop_release()
    assert eventually(lambda: not n.estop)
    b.request_lead()
    assert eventually(lambda: not a.leading and a.leader == 2 and b.term == 2)
    assert "leader" in roles and roles[-1] == "standby"
    n.stop()
    a.stop()
    b.stop()


def test_set_debug_level(bus, schema):
    a = make_host(bus, schema, host_id=1)
    b = make_host(bus, schema, host_id=2)
    a.start()
    b.start()
    n = FakeNode(bus, schema, "motor_controller", 22)
    assert a.wait_for_node(22, 1000)

    with pytest.raises(anv_can.HostError, match="not_leader"):
        b.set_debug_level(22, 2)
    a.request_lead()
    assert eventually(lambda: n.leader == 1)
    a.set_debug_level(22, 2, timeout_s=30)
    assert eventually(lambda: n.debug_level == 2 and n.debug_sets == 1)
    n.stop()
    a.stop()
    b.stop()


def test_event_callback(bus, schema):
    h = make_host(bus, schema)
    events = []
    h.set_callbacks(event=lambda e: events.append(e))
    h.start()
    n = FakeNode(bus, schema, "motor_controller", 23)
    assert h.wait_for_node(23, 1000)
    n.send_event(0x0101, detail=42, severity=1)  # can_error_passive, warning
    assert eventually(lambda: len(events) == 1)
    e = events[0]
    assert e["node"] == 23 and e["code"] == 0x0101 and e["detail"] == 42
    assert e["name"] == "can_error_passive" and e["severity"] == "warning" and not e["cleared"]
    n.stop()
    h.stop()


def test_id_assignment(bus, schema, tmp_path):
    table = tmp_path / "ids.txt"
    h = make_host(bus, schema, allocator="auto", id_first=40, id_last=45, id_table=str(table))
    got = []
    h.set_callbacks(id_assigned=lambda node, mac, type_id: got.append((node, mac, type_id)))
    h.start()
    n = FakeNode(bus, schema, "esp32_bench", 0, mac=0xA4CF12000009)
    assert eventually(lambda: n.id == 40)
    bench_id = schema.type("esp32_bench").type_id
    assert eventually(lambda: got and got[0] == (40, 0xA4CF12000009, bench_id))
    assert h.wait_for_node(40, 1000) and h.node(40).mac == 0xA4CF12000009
    assert h.id_table() == [(0xA4CF12000009, 40, bench_id)]
    assert "a4cf12000009 40" in table.read_text()
    n.stop()
    h.stop()
