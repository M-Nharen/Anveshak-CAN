"""Tests for the rover simulator (host/sim/rover_sim.py).

Run with `pytest host/sim`. Each test opens its own python-can virtual channel (named after
the test) so tests never see each other's traffic, and drives the simulator with
Simulator.step()/run_for() rather than a background thread, so there is no real-time flake
beyond the 100 ms command-timeout test, which only needs ordinary wall-clock sleeps.
"""
from __future__ import annotations

import itertools
import sys
import time
from pathlib import Path

import can
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rover_sim import (
    HW_TARGET_ESP32,
    HW_TARGET_ESP32S3,
    NODE_ID_REQUEST_ID,
    STATE_ARMED,
    STATE_DISARMED,
    STATE_ESTOP,
    STATE_FAULT,
    Simulator,
    load_db,
)

_channel_counter = itertools.count()


@pytest.fixture
def db():
    return load_db()


@pytest.fixture
def harness(db):
    """A (simulator, client_bus) pair sharing a fresh, test-private virtual channel."""
    channel = f"test-{next(_channel_counter)}"
    sim_bus = can.interface.Bus(interface="virtual", channel=channel, receive_own_messages=False)
    client_bus = can.interface.Bus(interface="virtual", channel=channel, receive_own_messages=False)
    sim = Simulator(sim_bus, db)
    yield sim, client_bus
    sim_bus.shutdown()
    client_bus.shutdown()


def send(bus, db, name, data):
    msg = db.get_message_by_name(name)
    bus.send(can.Message(arbitration_id=msg.frame_id, is_extended_id=False,
                          data=msg.encode(data, scaling=False)))


def recv_named(bus, db, name, timeout=0.5):
    """Receive frames until one matching `name` arrives, or time out (returns None).

    Decodes with scaling and named choices off, so callers compare against the same raw
    integers (and the STATE_* constants) the simulator works with internally.
    """
    deadline = time.monotonic() + timeout
    target_id = db.get_message_by_name(name).frame_id
    while time.monotonic() < deadline:
        frame = bus.recv(timeout=deadline - time.monotonic())
        if frame is None:
            return None
        if frame.arbitration_id == target_id:
            return db.decode_message(frame.arbitration_id, frame.data,
                                      decode_choices=False, scaling=False)
    return None


def arm(sim_or_bus, db, node_id=4, master=1, term=1):
    """Send NMT ARM targeted at `node_id` (default: drive node 1) from `master`/`term`."""
    bus = sim_or_bus
    send(bus, db, "NMT", {
        "TARGET_NODE": node_id, "COMMAND": 1, "MASTER_NODE_ID": master, "MASTER_TERM": term,
    })


def send_drive_cmd(bus, db, drive_num=1, target_a=500, target_b=500, sender_host=1, term_low=1):
    name = f"DRIVE{drive_num}_CMD"
    send(bus, db, name, {
        "SEQ": 0, "SENDER_HOST": sender_host, "TERM_LOW": term_low, "MODE": 2,
        "TARGET_A": target_a, "TARGET_B": target_b, "ACCEL_LIMIT": 0,
    })


def pump(sim, duration_s, interval_s=0.01):
    """Drive the simulator's event loop for `duration_s` without consuming client frames."""
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        sim.step(timeout=min(interval_s, max(deadline - time.monotonic(), 0)))


def test_disarmed_drive_node_ignores_commands(harness, db):
    sim, client = harness
    send_drive_cmd(client, db, drive_num=1)
    pump(sim, 0.05)
    assert sim.drive_nodes[1].state == STATE_DISARMED
    assert sim.drive_nodes[1].measured_a == 0


def test_arm_then_command_drives_feedback(harness, db):
    sim, client = harness
    arm(client, db, node_id=4, master=1, term=1)
    pump(sim, 0.03)
    assert sim.drive_nodes[1].state == STATE_ARMED

    send_drive_cmd(client, db, drive_num=1, target_a=1234, target_b=-500, sender_host=1, term_low=1)
    pump(sim, 0.03)

    send(client, db, "SYNC", {"CYCLE_COUNTER": 0})
    pump(sim, 0.03)

    fb = recv_named(client, db, "DRIVE1_FB_A", timeout=1.0)
    assert fb is not None
    assert fb["STATE"] == STATE_ARMED
    assert fb["SPEED_A"] == 1234
    assert fb["SPEED_B"] == -500


def test_disarms_on_command_timeout_within_100ms(harness, db):
    sim, client = harness
    arm(client, db, node_id=4, master=1, term=1)
    pump(sim, 0.03)
    send_drive_cmd(client, db, drive_num=1, sender_host=1, term_low=1)
    pump(sim, 0.03)
    assert sim.drive_nodes[1].state == STATE_ARMED

    start = time.monotonic()
    while sim.drive_nodes[1].state == STATE_ARMED and time.monotonic() - start < 0.5:
        sim.step(timeout=0.005)
    elapsed = time.monotonic() - start

    assert sim.drive_nodes[1].state == STATE_DISARMED
    assert elapsed < 0.25, f"node took {elapsed * 1000:.0f} ms to disarm after commands stopped"


def test_disarms_on_master_term_change(harness, db):
    sim, client = harness
    arm(client, db, node_id=4, master=1, term=1)
    pump(sim, 0.03)
    send_drive_cmd(client, db, drive_num=1, sender_host=1, term_low=1)
    pump(sim, 0.03)
    assert sim.drive_nodes[1].state == STATE_ARMED

    # A new term shows up without a fresh ARM: the node must drop out immediately.
    send_drive_cmd(client, db, drive_num=1, sender_host=2, term_low=2)
    pump(sim, 0.03)
    assert sim.drive_nodes[1].state == STATE_DISARMED


def test_estop_latches_and_release_returns_to_disarmed(harness, db):
    sim, client = harness
    arm(client, db, node_id=4, master=1, term=1)
    pump(sim, 0.03)
    assert sim.drive_nodes[1].state == STATE_ARMED

    send(client, db, "ESTOP", {"MAGIC": 0xE5})
    pump(sim, 0.03)
    assert sim.drive_nodes[1].state == STATE_ESTOP

    send(client, db, "ESTOP_RELEASE", {"MASTER_NODE_ID": 1, "MASTER_TERM": 1})
    pump(sim, 0.03)
    assert sim.drive_nodes[1].state == STATE_DISARMED


def test_overcurrent_fault_injection(harness, db):
    sim, client = harness
    sim.inject_overcurrent(2)
    assert sim.drive_nodes[2].state == STATE_FAULT
    send(client, db, "SYNC", {"CYCLE_COUNTER": 0})
    pump(sim, 0.03)
    fb = recv_named(client, db, "DRIVE2_FB_A", timeout=1.0)
    assert fb is not None
    assert fb["STATE"] == STATE_FAULT
    assert fb["FAULT_FLAGS"] & 0x01


def test_dropped_node_sends_nothing(harness, db):
    sim, client = harness
    sim.inject_drop(6)
    send(client, db, "SYNC", {"CYCLE_COUNTER": 0})
    pump(sim, 0.03)
    fb = recv_named(client, db, "DRIVE6_FB_A", timeout=0.3)
    assert fb is None
    hb = recv_named(client, db, "DRIVE6_NODE_HB", timeout=0.3)
    assert hb is None


def test_heartbeat_and_telemetry_rates(harness, db):
    sim, client = harness
    pump(sim, 0.35)
    hb_count = 0
    telem_count = 0
    deadline = time.monotonic() + 0.1
    while time.monotonic() < deadline:
        frame = client.recv(timeout=deadline - time.monotonic())
        if frame is None:
            break
        decoded_name = db.get_message_by_frame_id(frame.arbitration_id).name
        if decoded_name == "DRIVE1_NODE_HB":
            hb_count += 1
        elif decoded_name == "DRIVE1_TELEM_0":
            telem_count += 1
    # 10 Hz over ~0.35s of simulated activity should have produced a few of each.
    assert hb_count >= 1
    assert telem_count >= 1


def test_fpga_sends_sensor_frames(harness, db):
    sim, client = harness
    pump(sim, 0.05)
    frame = recv_named(client, db, "FPGA_SENSORS_A", timeout=1.0)
    assert frame is not None


# -- ESP32 bench nodes (automatic node IDs) ---------------------------------


@pytest.fixture
def bench(db):
    """A simulator with two unconfigured ESP32 bench nodes and nothing else, plus a client bus."""
    channel = f"test-{next(_channel_counter)}"
    sim_bus = can.interface.Bus(interface="virtual", channel=channel, receive_own_messages=False)
    client_bus = can.interface.Bus(interface="virtual", channel=channel, receive_own_messages=False)
    sim = Simulator(sim_bus, db, drives=False, fpga=False,
                    esp_macs=[(0xA4CF12000001, HW_TARGET_ESP32S3), (0x246F28000002, HW_TARGET_ESP32)])
    yield sim, client_bus
    sim_bus.shutdown()
    client_bus.shutdown()


def collect(sim, bus, seconds):
    frames = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        sim.step(timeout=0.002)
        while (f := bus.recv(timeout=0)) is not None:
            frames.append(f)
    return frames


def test_unconfigured_esp_nodes_ask_for_an_id_and_send_no_heartbeat(bench, db):
    sim, client = bench
    frames = collect(sim, client, 1.0)
    requests = [db.decode_message(f.arbitration_id, f.data, scaling=False, decode_choices=False)
                for f in frames if f.arbitration_id == NODE_ID_REQUEST_ID]
    assert {r["MAC"] for r in requests} == {0xA4CF12000001, 0x246F28000002}
    assert {r["HW_TARGET"] for r in requests} == {HW_TARGET_ESP32, HW_TARGET_ESP32S3}
    assert not [f for f in frames if (f.arbitration_id >> 8) == 6]  # no heartbeats while unconfigured


def test_assigned_esp_node_starts_heartbeats_and_sensors_at_its_id(bench, db):
    sim, client = bench
    send(client, db, "NODE_ID_ASSIGN_RPI", {"MAC": 0xA4CF12000001, "ASSIGNED_ID": 16, "FLAGS": 0})
    send(client, db, "NODE_ID_ASSIGN_RPI", {"MAC": 0x999, "ASSIGNED_ID": 17, "FLAGS": 0})  # not ours
    frames = collect(sim, client, 0.6)
    assert sim.esp_nodes[0].node_id == 16
    assert sim.esp_nodes[1].node_id is None
    hb = [f for f in frames if f.arbitration_id == 0x680]
    assert len(hb) >= 4
    decoded = db.get_message_by_name("DRIVE1_NODE_HB").decode(hb[-1].data, scaling=False, decode_choices=False)
    assert decoded["NODE_TYPE"] == 3
    assert decoded["STATE"] == STATE_DISARMED
    assert [f for f in frames if f.arbitration_id == 0x480]  # sensors at 0x400 + 8 * 16

    sim.esp_nodes[0].drop()
    assert not [f for f in collect(sim, client, 0.3) if f.arbitration_id == 0x680]
    sim.esp_nodes[0].forget()
    sim.esp_nodes[0].revive()
    assert [f for f in collect(sim, client, 0.8) if f.arbitration_id == NODE_ID_REQUEST_ID]


def test_assigned_esp_node_echoes_host_text(bench, db):
    sim, client = bench
    send(client, db, "NODE_ID_ASSIGN_RPI", {"MAC": 0xA4CF12000001, "ASSIGNED_ID": 16, "FLAGS": 0})
    collect(sim, client, 0.2)
    text = {"SEQ": 5, **{f"C{i}": b for i, b in enumerate(b"hello\0\0")}}
    msg = db.get_message_by_name("TEXT_TO_NODE_RPI")
    client.send(can.Message(arbitration_id=0x783, is_extended_id=False, data=msg.encode(text, scaling=False)))
    echoes = [f for f in collect(sim, client, 0.3) if f.arbitration_id == 0x785]
    assert len(echoes) == 1
    d = db.get_message_by_name("TEXT_FROM_NODE").decode(echoes[0].data, scaling=False, decode_choices=False)
    assert d["SEQ"] == 5 and bytes(d[f"C{i}"] for i in range(5)) == b"hello"
