from __future__ import annotations

from conftest import SCHEMA_DIR

from anv_schema.cli import main


def run(capsys, *args) -> tuple[int, str]:
    code = main(["--dir", str(SCHEMA_DIR), *args])
    out = capsys.readouterr()
    return code, out.out + out.err


def test_check(capsys):
    code, out = run(capsys, "check")
    assert code == 0 and "OK" in out


def test_show(capsys):
    code, out = run(capsys, "show")
    assert code == 0 and "esp32_bench" in out
    code, out = run(capsys, "show", "motor_controller")
    assert code == 0 and "setpoint" in out and "kp_0" in out and "heartbeat_hz" in out
    code, out = run(capsys, "show", "nope")
    assert code == 1


def test_encode_then_decode(capsys):
    code, out = run(capsys, "encode", "motor_controller", "setpoint", "mode=velocity",
                    "target=[1.5,-2.25]", "--src", "1", "--dst", "20")
    assert code == 0
    frame = out.split()[0]
    can_id, data = frame.split("#")
    code, out = run(capsys, "decode", f"0x{can_id}", data, "--type", "motor_controller")
    assert code == 0
    assert "setpoint (1 -> 20" in out and "velocity" in out


def test_id(capsys):
    code, out = run(capsys, "id", "0x14000610")
    assert code == 0
    assert "port 6 heartbeat" in out and "src 16" in out


def test_dbc(capsys, tmp_path):
    out_file = tmp_path / "bench.dbc"
    code, _ = run(capsys, "dbc", str(SCHEMA_DIR / "buses" / "bench.yaml"), "-o", str(out_file))
    assert code == 0
    text = out_file.read_text()
    assert "bench_a_status" in text and "rpi_to_motors_front_setpoint" in text
