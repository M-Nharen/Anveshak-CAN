"""anv-build with a stand-in arduino-cli (the real one and the esp32 core run in CI's
`anv-build` job): what it is given, the work copy it builds, and flashing per chip over CAN."""

from __future__ import annotations

import io
import json
import os
import stat
import textwrap
from pathlib import Path

import pytest
from conftest import FakeNode
from test_flash_cli import FakeOtaNode

import anv_can
from anv_can import build_cli

REPO = Path(__file__).resolve().parents[4]
BENCH = REPO / "firmware" / "nodes" / "bench_esp32_arduino"

# Logs each call as JSON, checks the work copy, writes an image that names the board it was
# built for. A sketch containing BROKEN fails like a compiler error.
FAKE_CLI = textwrap.dedent("""\
    #!/usr/bin/env python3
    import json, os, sys, pathlib
    a = sys.argv[1:]
    log = pathlib.Path(os.environ["FAKE_CLI_LOG"])
    with log.open("a") as f:
        f.write(json.dumps(a) + "\\n")
    if a[:1] == ["version"]:
        print("arduino-cli  Version: 1.3.1"); sys.exit(0)
    if a[:2] == ["core", "list"]:
        print("ID  Installed Latest Name"); print(os.environ.get("FAKE_CORE", "")); sys.exit(0)
    assert a[0] == "compile", a
    sketch = pathlib.Path(a[-1]); build = pathlib.Path(a[a.index("--build-path") + 1])
    fqbn = a[a.index("--fqbn") + 1]
    schema = sketch / "src" / "schema"
    names = sorted(p.name for p in schema.iterdir())
    if any("BROKEN" in p.read_text() for p in sketch.glob("*.cpp")):
        print(f"{sketch}/user_app.cpp:3:1: error: 'BROKEN' does not name a type"); sys.exit(1)
    build.mkdir(parents=True, exist_ok=True)
    image = json.dumps({"fqbn": fqbn, "schema": names}).encode()
    (build / (sketch.name + ".ino.bin")).write_bytes(image)
    print("Sketch uses 912345 bytes (69%) of program storage space.")
    """)


@pytest.fixture
def fake_cli(tmp_path, monkeypatch):
    cli = tmp_path / "arduino-cli"
    cli.write_text(FAKE_CLI)
    cli.chmod(cli.stat().st_mode | stat.S_IEXEC)
    log = tmp_path / "calls.jsonl"
    monkeypatch.setenv("ANV_ARDUINO_CLI", str(cli))
    monkeypatch.setenv("FAKE_CLI_LOG", str(log))
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    monkeypatch.delenv("ANV_REPO", raising=False)

    def calls() -> list[list[str]]:
        return [json.loads(x) for x in log.read_text().splitlines()] if log.exists() else []

    return calls


def run(*argv: str, open_link=None) -> tuple[int, str]:
    out = io.StringIO()
    code = build_cli.main([*argv], open_link, out)
    return code, out.getvalue()


def test_finds_types_and_repo():
    assert build_cli.schema_types(BENCH) == ["esp32_bench"]
    assert build_cli.find_repo(BENCH) == REPO


def test_build_only(fake_cli, tmp_path):
    code, out = run(str(BENCH), "--chip", "esp32s3", "--no-gen", "-o", str(tmp_path))
    assert code == 0, out
    image = tmp_path / "bench_esp32_arduino-esp32s3.bin"
    built = json.loads(image.read_bytes())
    assert built["fqbn"] == "esp32:esp32:esp32s3"
    # the type's own files and the two headers, never anv_protocol.c (the library compiles it)
    assert built["schema"] == [
        "anv_esp32_bench.c",
        "anv_esp32_bench.h",
        "anv_protocol.h",
        "anv_schema.h",
    ]
    (call,) = fake_cli()
    libs = [call[i + 1] for i, x in enumerate(call) if x == "--library"]
    assert [Path(x).name for x in libs] == ["anv_link", "anv_proto", "anv_node"]
    assert all(Path(x).parent == REPO / "firmware" / "lib" for x in libs)
    assert "--upload" not in call
    assert "anv-flash flash can1" in out


def test_work_copy_is_incremental_and_never_touches_the_sketch(fake_cli, tmp_path):
    sketch = tmp_path / "src_tree" / "my_node"
    sketch.mkdir(parents=True)
    (sketch / "my_node.ino").write_text(
        '#include "src/schema/anv_esp32_bench.h"\nvoid setup(){}\nvoid loop(){}\n'
    )
    (sketch / "helper.cpp").write_text("int x;\n")
    args = (str(sketch), "--chip", "esp32", "--no-gen", "--repo", str(REPO))
    assert run(*args)[0] == 0
    work = tmp_path / "cache" / "anveshak" / "build" / "my_node-esp32" / "my_node"
    before = (work / "helper.cpp").stat().st_mtime_ns
    (sketch / "my_node.ino").write_text((sketch / "my_node.ino").read_text() + "// edit\n")
    assert run(*args)[0] == 0
    assert (work / "helper.cpp").stat().st_mtime_ns == before  # unchanged file not rewritten
    assert "// edit" in (work / "my_node.ino").read_text()
    (sketch / "helper.cpp").unlink()
    assert run(*args)[0] == 0
    assert not (work / "helper.cpp").exists()  # deleted files go from the work copy too
    assert not (sketch / "src").exists()  # the user's folder stays as it was


def test_usb_upload(fake_cli):
    code, out = run(str(BENCH), "--chip", "esp32", "--usb", "/dev/ttyUSB0", "--no-gen")
    assert code == 0, out
    (call,) = fake_cli()
    assert call[call.index("--port") + 1] == "/dev/ttyUSB0" and "--upload" in call
    assert call[call.index("--fqbn") + 1] == "esp32:esp32:esp32"


def test_compile_error_and_hints(fake_cli, tmp_path):
    sketch = tmp_path / "bad"
    sketch.mkdir()
    (sketch / "bad.ino").write_text('#include "src/schema/anv_esp32_bench.h"\n')
    (sketch / "user_app.cpp").write_text("\n\nBROKEN\n")
    code, out = run(str(sketch), "--chip", "esp32", "--no-gen", "--repo", str(REPO))
    assert code == 1 and "error: 'BROKEN'" in out
    assert "64-bit Raspberry Pi OS" in build_cli._hint(
        ["fork/exec ... ld-linux-aarch64.so.1: no such file"]
    )
    assert "dialout" in build_cli._hint(["Permission denied", "/dev/ttyUSB0"])


def test_missing_type_and_missing_cli(fake_cli, tmp_path, monkeypatch, capsys):
    sketch = tmp_path / "x"
    sketch.mkdir()
    (sketch / "x.ino").write_text('#include "src/schema/anv_no_such_type.h"\n')
    assert run(str(sketch), "--chip", "esp32", "--no-gen", "--repo", str(REPO))[0] == 1
    assert "no_such_type" in capsys.readouterr().err
    (sketch / "x.ino").write_text("void setup(){}\n")
    assert run(str(sketch), "--chip", "esp32", "--no-gen", "--repo", str(REPO))[0] == 1
    assert "includes no node type" in capsys.readouterr().err
    monkeypatch.setenv("ANV_ARDUINO_CLI", "")
    monkeypatch.setenv("PATH", str(tmp_path / "empty"))
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    assert run(str(BENCH), "--chip", "esp32", "--no-gen")[0] == 1
    assert "setup.sh" in capsys.readouterr().err


def test_check(fake_cli, monkeypatch):
    monkeypatch.setenv("FAKE_CORE", "esp32:esp32 3.3.12    3.3.12 esp32")
    code, out = run("--check")
    assert code == 0 and "esp32 core: 3.3.12" in out
    monkeypatch.setenv("FAKE_CORE", "")
    code, out = run("--check")
    assert code == 1 and "NOT installed" in out


def test_can_builds_once_per_chip_and_flashes_each_node(fake_cli):
    schema = anv_can.load_schema()
    type_id = schema.type("esp32_bench").type_id
    bus = anv_can.TestBus()
    nodes = [FakeNode(bus, schema, "esp32_bench", nid) for nid in (17, 18, 19)]
    for n, chip in zip(nodes, (1, 2, 2), strict=True):
        n.info_pages["chip"] = chip
    ota = {
        nid: FakeOtaNode(bus, schema, nid, type_id, chip=chip)
        for nid, chip in ((17, 1), (18, 2), (19, 2))
    }
    try:
        code, out = run(
            str(BENCH),
            "all",
            "--can",
            "x",
            "--wait",
            "1",
            "--no-gen",
            open_link=lambda _name: bus.connect(),
        )
        assert code == 0, out
        assert "3/3 node(s) flashed" in out
        fqbns = [c[c.index("--fqbn") + 1] for c in fake_cli() if c[0] == "compile"]
        assert sorted(fqbns) == ["esp32:esp32:esp32", "esp32:esp32:esp32s3"]  # one build per chip
        assert json.loads(ota[17].image)["fqbn"] == "esp32:esp32:esp32"
        assert json.loads(ota[18].image)["fqbn"] == "esp32:esp32:esp32s3"
        assert ota[19].image == ota[18].image and all(o.commits == 1 for o in ota.values())
    finally:
        for n in nodes:
            n.stop()
        for o in ota.values():
            o.stop()


def test_argument_errors(fake_cli):
    for argv in (
        [str(BENCH)],
        [str(BENCH), "17", "--chip", "esp32"],
        [str(BENCH), "--can", "can1"],
        [str(BENCH), "--usb", "/dev/x", "--can", "c", "1"],
    ):
        with pytest.raises(SystemExit):
            build_cli.main(argv)
    assert os.environ["ANV_ARDUINO_CLI"]


def test_can_refuses_a_board_of_another_type(fake_cli):
    """The image goes out labelled with the sketch's type: a board running another type says no."""
    schema = anv_can.load_schema()
    other = schema.type("esp32_bench").type_id + 1
    bus = anv_can.TestBus()
    n = FakeNode(bus, schema, "esp32_bench", 21)
    o = FakeOtaNode(bus, schema, 21, other)  # this board's firmware is another type
    try:
        code, out = run(
            str(BENCH),
            "21",
            "--can",
            "x",
            "--wait",
            "1",
            "--no-gen",
            open_link=lambda _name: bus.connect(),
        )
        assert code == 1 and "wrong type or chip" in out
        assert o.commits == 0
    finally:
        n.stop()
        o.stop()
