import json
import os
import socket
import sys
import tempfile
from pathlib import Path

import anv_can
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fakes import FakeNode, FakeOtaNode, TextEcho, app_image, wait_for  # noqa: E402

from anv_bench_gateway.bridge import Bridge  # noqa: E402
from anv_bench_gateway.gateway import BenchGateway, GatewayConfig  # noqa: E402


@pytest.fixture(scope="module")
def schema():
    return anv_can.load_schema()


@pytest.fixture
def bus():
    return anv_can.TestBus()


@pytest.fixture
def gw(bus, schema, tmp_path):
    g = BenchGateway(bus.connect, schema, GatewayConfig(host_id=2, id_table=str(tmp_path / "ids.txt")))
    g.start()
    yield g
    g.stop()


def node_state(gw, node_id):
    return next((n for n in gw.state()["nodes"] if n["node_id"] == node_id), None)


def test_new_board_gets_an_id_and_shows_up_with_its_values(gw, bus, schema):
    board = FakeNode(bus, schema, "esp32_bench", 0, mac=0xA4CF12000001)
    try:
        assert wait_for(lambda: board.id == 16)
        assert wait_for(lambda: (n := node_state(gw, 16)) is not None and n["alive"] and n["hw_name"])
        n = node_state(gw, 16)
        assert n["type_name"] == "esp32_bench" and n["state_name"] == "IDLE" and n["state"] == 1
        assert n["mac"] == "a4:cf:12:00:00:01" and n["hw_name"] == "ESP32"
        assert wait_for(
            lambda: (
                {v["name"] for v in node_state(gw, 16)["values"]}
                >= {"chip_temp", "free_heap", "rx_frames", "tx_frames"}
            )
        )
        st = gw.state()
        kinds = [e["type"] for e in st["events"]]
        assert "NODE_ID_ASSIGNED" in kinds and "NODE_ALIVE" in kinds
        assert st["online_count"] == 1 and st["id_table"][0]["node_id"] == 16
        json.dumps(st)  # the bridge sends it as JSON
        board.stop()
        assert wait_for(lambda: not node_state(gw, 16)["alive"])
        assert "NODE_MISSING" in [e["type"] for e in gw.state()["events"]]
    finally:
        board.stop()


def test_text_is_echoed_and_node_text_arrives(gw, bus, schema):
    echo = TextEcho(bus, schema, 20)
    try:
        ok, _, seq = gw.send_text(20, "hello")
        assert ok and seq >= 1
        assert wait_for(
            lambda: any(
                t["dir"] == "rx" and t["seq"] == seq and t["text"] == "hello" for t in gw.state()["texts"]
            )
        )
        echo.say("hi pi")
        assert wait_for(
            lambda: any(
                t["dir"] == "rx" and t["seq"] == 0 and t["text"] == "hi pi" for t in gw.state()["texts"]
            )
        )
        assert gw.send_text(20, "far too long")[0] is False
        assert gw.send_text(0, "all")[0] is True
        assert wait_for(lambda: "all" in echo.received)
    finally:
        echo.stop()


def test_forget_frees_the_id(gw, bus, schema):
    board = FakeNode(bus, schema, "esp32_bench", 0, mac=0xA4CF12000002)
    try:
        assert wait_for(lambda: board.id == 16)
        assert gw.forget_node(16) == (True, "ok")
        assert gw.state()["id_table"] == []
        assert gw.forget_node(16)[0] is False
    finally:
        board.stop()


def test_flash_one_node_over_can(gw, bus, schema):
    t = schema.type("esp32_bench")
    node = FakeNode(bus, schema, "esp32_bench", 20)
    ota = FakeOtaNode(bus, schema, 20, t.type_id, chip=1, peer=2)
    try:
        assert wait_for(lambda: node_state(gw, 20) is not None)
        image = app_image(chip_id=0)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(image)
        assert gw.flash(20, f.name) == (True, "ok")
        assert gw.flash(20, f.name)[0] is False or gw.state()["flash"]["state"] != "running"
        assert wait_for(lambda: gw.state()["flash"]["state"] != "running", timeout=10)
        fl = gw.state()["flash"]
        assert fl["state"] == "done", fl
        assert ota.image == image and ota.commits == 1
        os.unlink(f.name)
    finally:
        ota.stop()
        node.stop()


def test_flash_refuses_the_other_chips_image(gw, bus, schema):
    node = FakeNode(bus, schema, "esp32_bench", 21)
    try:
        assert wait_for(lambda: node_state(gw, 21) is not None)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(app_image(chip_id=9))  # an S3 image; the fake node is a WROOM (chip 1)
        gw.flash(21, f.name)
        assert wait_for(lambda: gw.state()["flash"]["state"] == "failed")
        assert "ESP32-S3" in gw.state()["flash"]["msg"]
        os.unlink(f.name)
    finally:
        node.stop()


def test_bridge_speaks_the_old_gateway_protocol(gw, tmp_path):
    path = str(tmp_path / "gw.sock")
    bridge = Bridge(gw, path)
    bridge.start()
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.connect(path)
            f = s.makefile("rw")

            def call(**req):
                f.write(json.dumps(req) + "\n")
                f.flush()
                return json.loads(f.readline())

            assert call(id=1, cmd="ping")["ok"]
            st = call(id=2, cmd="state")
            assert st["ok"] and st["id"] == 2
            for key in (
                "role",
                "role_name",
                "term",
                "believed_master",
                "bus",
                "peers",
                "nodes",
                "events",
                "texts",
            ):
                assert key in st["state"]
            assert call(id=3, cmd="send_text", node_id=0, text="hey")["seq"] >= 1
            assert call(id=4, cmd="set_targets")["ok"] is False
            assert call(id=5, cmd="nope")["ok"] is False
            assert call(id=6, cmd="estop")["ok"] and gw.state()["estop_latched"]
    finally:
        bridge.stop()
