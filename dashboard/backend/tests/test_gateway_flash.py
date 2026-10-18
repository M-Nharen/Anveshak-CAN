"""Flashing through the v2 bench gateway: .bin straight through, .cpp built (by a stand-in for
arduino-cli) and cached, and the v2 node fields on the Bench page."""

import asyncio
import stat
import sys
from pathlib import Path

from anv_dashboard import firmware_build, views
from anv_dashboard.flash import GatewayFlash
from anv_dashboard.settings import Settings
from fakes import FakeBridge, make_node, make_state

FAKE_CLI = """#!{python}
# Stands in for arduino-cli: fails on a file containing BROKEN, else writes a fake image.
import sys, pathlib
args = sys.argv[1:]
out = pathlib.Path(args[args.index("--output-dir") + 1])
sketch = pathlib.Path(args[-1])
calls = pathlib.Path({calls!r})
calls.write_text(calls.read_text() + args[args.index("--fqbn") + 1] + "\\n" if calls.exists() else
                 args[args.index("--fqbn") + 1] + "\\n")
src = (sketch / "user_app.cpp").read_text()
print("Compiling sketch...")
if "BROKEN" in src:
    print("user_app.cpp:3:1: error: 'BROKEN' does not name a type")
    sys.exit(1)
out.mkdir(parents=True, exist_ok=True)
(out / "bench_esp32_arduino.ino.bin").write_bytes(b"\\xe9" + src.encode())
print("Sketch uses 300000 bytes")
"""


def make_repo(root: Path) -> Path:
    """The parts of the repository the build reads."""
    sketch = root / "firmware" / "nodes" / "bench_esp32_arduino"
    sketch.mkdir(parents=True)
    (sketch / "bench_esp32_arduino.ino").write_text("// sketch\n")
    (sketch / "user_app.cpp").write_text("// default\n")
    for lib in firmware_build.LIBRARIES:
        (root / "firmware" / "lib" / lib).mkdir(parents=True)
        (root / "firmware" / "lib" / lib / "x.c").write_text(lib)
    (root / "schema" / "generated" / "c").mkdir(parents=True)
    return root


def setup(tmp_path: Path, chip: int = 1):
    repo = make_repo(tmp_path / "repo")
    calls = tmp_path / "cli_calls.txt"
    cli = tmp_path / "arduino-cli"
    cli.write_text(FAKE_CLI.format(python=sys.executable, calls=str(calls)))
    cli.chmod(cli.stat().st_mode | stat.S_IEXEC)
    s = Settings(password="pw", firmware_dir=tmp_path / "fw", repo_dir=repo, arduino_cli=str(cli),
                 build_cache_dir=tmp_path / "cache")  # fmt: skip
    state = make_state(nodes=[make_node(16, hw_target=chip, hw_name="ESP32" if chip == 1 else "ESP32-S3")])
    bridge = FakeBridge(state)
    return GatewayFlash(s, bridge, lambda: bridge.state_value), bridge, calls, repo


async def finished(job: GatewayFlash) -> dict:
    for _ in range(300):
        snap = job.snapshot()
        if snap and snap["state"] != "running":
            return snap
        await asyncio.sleep(0.02)
    raise AssertionError("never finished")


def test_bin_upload_goes_straight_to_the_gateway(tmp_path):
    async def main():
        job, bridge, calls, _ = setup(tmp_path)
        assert await job.start(16, b"\xe9image", "bench.ino.bin") is None
        snap = await finished(job)
        assert snap["state"] == "done" and "new image" in snap["msg"]
        assert bridge.calls[0][0] == "flash" and bridge.calls[0][1][0] == 16
        assert not calls.exists()  # nothing was built

    asyncio.run(main())


def test_cpp_is_built_for_the_nodes_chip_then_cached(tmp_path):
    async def main():
        job, bridge, calls, _ = setup(tmp_path, chip=2)
        src = b"void user_setup() {}\nvoid user_loop(unsigned long) {}\nbool user_owns_led() { return false; }\n"
        await job.start(16, src, "user_app.cpp")
        snap = await finished(job)
        assert snap["state"] == "done", snap
        assert calls.read_text().split() == ["esp32:esp32:esp32s3"]
        built = Path(bridge.calls[-1][1][1])
        assert built.read_bytes() == b"\xe9" + src and built.name == "bench_esp32_arduino.ino.bin"
        # Same file again: no build, the cached image is flashed.
        await job.start(16, src, "user_app.cpp")
        assert (await finished(job))["state"] == "done"
        assert calls.read_text().split() == ["esp32:esp32:esp32s3"]
        assert Path(bridge.calls[-1][1][1]) == built
        # A different file builds afresh.
        await job.start(16, src + b"// changed\n", "user_app.cpp")
        assert (await finished(job))["state"] == "done"
        assert len(calls.read_text().split()) == 2

    asyncio.run(main())


def test_a_source_change_elsewhere_invalidates_the_cache(tmp_path):
    repo = make_repo(tmp_path / "repo")
    before = firmware_build.source_hash(repo)
    (repo / "firmware" / "lib" / "anv_node" / "x.c").write_text("changed")
    assert firmware_build.source_hash(repo) != before
    (repo / "firmware" / "nodes" / "bench_esp32_arduino" / "user_app.cpp").write_text("ignored")
    assert firmware_build.cache_key(b"a", "f", before) != firmware_build.cache_key(b"b", "f", before)


def test_compile_errors_are_shown_and_nothing_is_flashed(tmp_path):
    async def main():
        job, bridge, _, _ = setup(tmp_path)
        await job.start(16, b"BROKEN\n", "user_app.cpp")
        snap = await finished(job)
        assert snap["state"] == "failed" and "does not name a type" in snap["msg"]
        assert not [c for c in bridge.calls if c[0] == "flash"]

    asyncio.run(main())


def test_unknown_chip_and_bad_file_types_are_refused(tmp_path):
    async def main():
        job, _, _, _ = setup(tmp_path, chip=0)
        await job.start(16, b"x", "user_app.cpp")
        assert "chip isn't known" in (await finished(job))["msg"]
        assert "upload a .bin" in (await job.start(16, b"x", "notes.txt") or "")

    asyncio.run(main())


def test_bench_view_shows_v2_fields():
    n = make_node(16, state=1, state_name="IDLE", type_name="esp32_bench", hw_name="ESP32",
                  values=[{"name": "chip_temp", "value": 41.5, "unit": "degC"}])  # fmt: skip
    events = [{"type": "NODE_EVENT", "time_ns": 1, "age_ms": 10.0, "node_id": 16, "name": "NODE16",
               "event": "sensor_fault", "severity": "error", "cleared": False}]  # fmt: skip
    b = views.bench_view(make_state(nodes=[n], events=events))
    tile = b["nodes"][0]
    assert tile["state"] == "IDLE" and tile["type"] == "esp32_bench" and tile["values"][0]["unit"] == "degC"
    assert b["events"][0]["text"] == "NODE16: sensor_fault"


def test_s3_builds_use_the_configured_board_options(tmp_path):
    async def main():
        job, _, calls, _ = setup(tmp_path, chip=2)
        job.settings.fqbn_esp32s3 = "esp32:esp32:esp32s3:CDCOnBoot=cdc"
        await job.start(16, b"// s3 app\n", "user_app.cpp")
        assert (await finished(job))["state"] == "done"
        assert calls.read_text().split() == ["esp32:esp32:esp32s3:CDCOnBoot=cdc"]
        # The options are part of the cache key: the plain S3 build is a different image.
        job.settings.fqbn_esp32s3 = "esp32:esp32:esp32s3"
        await job.start(16, b"// s3 app\n", "user_app.cpp")
        assert (await finished(job))["state"] == "done"
        assert len(calls.read_text().split()) == 2

    asyncio.run(main())


def test_fqbn_settings_from_the_environment():
    s = Settings.from_env({"ANV_DASH_FQBN_ESP32S3": "esp32:esp32:esp32s3:CDCOnBoot=cdc"})
    assert s.fqbn_esp32s3.endswith("CDCOnBoot=cdc") and s.fqbn_esp32 == "esp32:esp32:esp32"
