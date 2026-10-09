from fakes import make_node, make_state

from anv_dashboard import views
from anv_dashboard.settings import Settings


def test_header_prompts_for_a_leader():
    quiet = {"source": "none", "mine": False, "taken": False}
    h = views.header_view(
        make_state(believed_master=0, role=1, role_name="STANDBY"), quiet
    )
    assert h["prompt"] == "No leader — choose one" and not h["is_master"]
    h = views.header_view(
        make_state(believed_master=2, role=1, leader_lost=True), quiet
    )
    assert "lost" in h["prompt"]
    h = views.header_view(make_state(), quiet)
    assert (
        h["prompt"] is None
        and h["leader_name"] == "Orin"
        and h["term"] == 3
        and h["is_master"]
    )


def test_header_without_a_gateway():
    h = views.header_view(None, {"source": "none"})
    assert h["gateway"] is False


def test_node_health_colours():
    assert views.node_health(make_node(4))[0] == "green"
    assert views.node_health(make_node(4, alive=False))[0] == "red"
    assert views.node_health(make_node(4, state=3))[0] == "red"
    assert views.node_health(make_node(4, age_ms=350.0))[0] == "yellow"
    assert views.node_health(make_node(4, tec=100))[0] == "yellow"
    assert views.node_health(make_node(4, reboot_count=1))[0] == "yellow"
    hot = make_node(4, telem={"supply_v": 24.0, "temp_mcu": 85})
    assert views.node_health(hot)[0] == "red"
    faulty = make_node(4, fb={"state": 3, "fault_flags": 3, "speed_a": 0, "speed_b": 0})
    colour, reasons = views.node_health(faulty)
    assert (
        colour == "red"
        and "overcurrent A" in reasons[0]
        and "overcurrent B" in reasons[0]
    )
    assert views.node_health(make_node(4, seen=False))[0] == "grey"


def test_nodes_view_rows():
    rows = views.nodes_view(make_state())
    assert len(rows) == 6
    r = rows[0]
    assert (
        r["state"] == "DISARMED"
        and r["fw_build_id"] == 0xBEEF
        and r["temps"]["mcu"] == 35
    )
    assert r["health"] == "green" and r["age_ms"] == 40


def test_overview_bus_load_and_overruns():
    rates = views.RateTracker()
    rates.update(0.0, {"tx_frames": 0, "error_frames": 0})
    rates.update(1.0, {"tx_frames": 400, "error_frames": 5})
    v = views.overview_view(make_state(), rates, Settings())
    assert v["frames_per_s"] == 1000.0  # 600 received + 400 sent
    assert abs(v["bus_load_pct"] - 1000 * 111 / 500_000 * 100) < 0.1
    assert (
        v["error_frames_per_s"] == 5.0 and v["overruns"] == 2 and v["bus_state"] == "ok"
    )
    bad = make_state()
    bad["bus"]["bus_off"] = True
    assert views.overview_view(bad, rates, Settings())["bus_state"] == "BUS-OFF"


def test_rate_tracker_ignores_counter_resets():
    r = views.RateTracker()
    r.update(0.0, {"x": 100})
    r.update(1.0, {"x": 150})
    assert r.rates["x"] == 50
    r.update(2.0, {"x": 10})  # the gateway restarted
    assert r.rates["x"] == 50  # unchanged, not negative


def test_drive_view_pairs_target_and_measurement():
    st = make_state()
    st["nodes"][1]["fb"]["speed_a"] = 1.5
    st["nodes"][1]["state"] = 2
    rows = views.drive_view(st, [0, 2.0, 0, 0, 0, 0])
    assert (
        rows[1]["target"] == 2.0
        and rows[1]["measured_a"] == 1.5
        and rows[1]["state"] == "ARMED"
    )
    gone = make_state()
    gone["nodes"] = []
    assert views.drive_view(gone, [0] * 6)[0]["measured_a"] is None
