from __future__ import annotations

import io
import os
import shutil
import subprocess
import time

import pytest

import anv_can
from anv_can import cli


@pytest.fixture(scope="module")
def schema():
    return anv_can.load_schema()


def test_send_builds_frames(schema):
    args = ["motor_controller", "setpoint", "mode=velocity", "target=[1.5,-2]"]
    f = cli.build_frame(schema, args, src=1, dst=20, raw=False)
    assert str(f) == "04504001#02960038FF0000"  # the example in schema/README.md
    args = ["protocol", "nmt", "command=arm", "term=3"]
    f = cli.build_frame(schema, args, src=1, dst=20, raw=False)
    p = schema.parse_id(f.id)
    assert (p["port"], p["dst"], p["src"]) == (schema.protocol.message("nmt").port, 20, 1)
    assert f.data == bytes([1, 3, 0])
    assert str(cli.build_frame(schema, ["123#AB"], 1, 0, False)) == "123#AB"
    with pytest.raises(KeyError):
        cli.build_frame(schema, ["no_type", "x"], 1, 0, False)


def test_dump_learns_types_from_heartbeats(schema):
    bus = anv_can.TestBus()
    tx, rx = bus.connect(), bus.connect()
    mc = schema.type("motor_controller")
    fb = mc.message("feedback")
    fb_frame = anv_can.Frame(anv_can.make_id(schema, fb, src=20),
                             anv_can.encode(fb, {"speed": [1.0, 2.0]}))
    hb = schema.protocol.message("heartbeat")
    tx.send(fb_frame)  # before the heartbeat: unknown
    tx.send(anv_can.Frame(anv_can.make_id(schema, hb, src=20),
                          anv_can.encode(hb, {"state": "idle", "type_id": mc.type_id})))
    tx.send(fb_frame)
    tx.send(anv_can.Frame(0x123, b"\x00", extended=False))
    out = io.StringIO()
    assert cli.dump(rx, schema, out, count=4, duration_s=2) == 4
    lines = out.getvalue().splitlines()
    assert "unknown (sender type not known yet)" in lines[0]
    assert "protocol.heartbeat" in lines[1] and "state=idle" in lines[1]
    assert "motor_controller.feedback" in lines[2] and "speed=[1,2]" in lines[2]
    assert "11-bit" in lines[3]


def test_main_with_test_bus(schema, capsys):
    bus = anv_can.TestBus()
    watcher = bus.connect()
    links = []

    def opener(_name):
        links.append(bus.connect())
        return links[-1]

    assert cli.main(["send", "x", "protocol", "estop", "reason=2", "--src", "5"], opener) == 0
    f = watcher.recv(100)
    assert f.data == b"\xe5\x02" and schema.parse_id(f.id)["src"] == 5
    assert cli.main(["send", "x", "protocol", "estop", "bogus=1"], opener) == 2
    assert "no field" in capsys.readouterr().err


def _vcan_up() -> bool:
    return os.path.exists("/sys/class/net/vcan0")


@pytest.mark.skipif(not _vcan_up() or shutil.which("anv-can") is None, reason="needs vcan0")
def test_vcan_dump_and_send():
    """End to end on a real SocketCAN interface (CI loads vcan where the runner allows it)."""
    dump = subprocess.Popen(["anv-can", "dump", "vcan0", "--count", "2", "--duration", "10"],
                            stdout=subprocess.PIPE, text=True)
    time.sleep(0.5)
    subprocess.run(["anv-can", "send", "vcan0", "protocol", "sync", "counter=7", "period_ms=20"],
                   check=True)
    subprocess.run(["anv-can", "send", "vcan0", "motor_controller", "setpoint", "mode=brake",
                    "--dst", "20"], check=True)
    out, _ = dump.communicate(timeout=15)
    assert "protocol.sync" in out and "counter=7" in out
    assert "port 64: unknown" in out  # no heartbeat from node 20 yet
