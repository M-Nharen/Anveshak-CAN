"""Firmware updates from the dashboard: the job runner (with a stand-in for anv-flash) and the upload endpoint."""

import asyncio
import json
import sys
from pathlib import Path

from fastapi.testclient import TestClient

from anv_dashboard.app import create_app
from anv_dashboard.flash import FlashJobs
from anv_dashboard.settings import Settings
from fakes import FakeBridge


def fake_tool(lines: list[dict], rc: int = 0, sleep: float = 0.0):
    """A command that prints anv-flash style JSON progress and exits with rc."""
    script = "import json,sys,time\n" + "".join(
        f"print(json.dumps({line!r}), flush=True); time.sleep({sleep})\n" for line in lines
    ) + f"sys.exit({rc})\n"
    return lambda node_id, path: [sys.executable, "-c", script]


def settings(tmp_path: Path, **over) -> Settings:
    return Settings(password="pw", firmware_dir=tmp_path, **over)


async def wait_finished(jobs: FlashJobs) -> dict:
    for _ in range(200):
        snap = jobs.snapshot()
        if snap and snap["state"] != "running":
            return snap
        await asyncio.sleep(0.02)
    raise AssertionError("flash job never finished")


def test_a_successful_flash_reports_progress_then_done(tmp_path):
    async def main():
        jobs = FlashJobs(settings(tmp_path), fake_tool([
            {"phase": "check", "pct": 0, "msg": "image ok"},
            {"phase": "write", "pct": 50.0, "msg": ""},
            {"phase": "done", "pct": 100.0, "msg": "node 16 is running the new image (build beef)"},
        ]))  # fmt: skip
        assert await jobs.start(16, b"\xe9" * 100, "bench.ino.bin") is None
        snap = await wait_finished(jobs)
        assert snap["state"] == "done" and snap["node_id"] == 16 and snap["file"] == "bench.ino.bin"
        assert "new image" in snap["msg"]
        assert not (tmp_path / "node16.bin").exists()  # the upload is removed afterwards

    asyncio.run(main())


def test_an_error_from_the_tool_marks_the_job_failed(tmp_path):
    async def main():
        jobs = FlashJobs(settings(tmp_path), fake_tool([{"phase": "error", "pct": 0, "msg": "node 16 is armed"}], rc=1))
        await jobs.start(16, b"x", "f.bin")
        snap = await wait_finished(jobs)
        assert snap["state"] == "failed" and snap["msg"] == "node 16 is armed"

    asyncio.run(main())


def test_one_flash_at_a_time_and_cancel(tmp_path):
    async def main():
        jobs = FlashJobs(settings(tmp_path), fake_tool([{"phase": "write", "pct": 1.0, "msg": ""}] * 50, sleep=0.1))
        await jobs.start(16, b"x", "f.bin")
        assert "being flashed" in (await jobs.start(18, b"x", "g.bin") or "")
        await asyncio.sleep(0.2)
        assert jobs.cancel()
        snap = await wait_finished(jobs)
        assert snap["state"] == "failed"
        assert not jobs.cancel()

    asyncio.run(main())


def test_upload_endpoint_needs_login_and_starts_the_job(tmp_path, monkeypatch):
    s = settings(tmp_path, poll_hz=50.0)
    app = create_app(s, FakeBridge())
    started = []

    async def fake_start(node_id, data, filename):
        started.append((node_id, len(data), filename))

    monkeypatch.setattr(app.state.hub.flash, "start", fake_start)
    with TestClient(app) as c:
        assert c.post("/api/flash/16", content=b"\xe9" * 10).status_code == 401
        token = c.post("/api/login", json={"password": "pw"}).json()["token"]
        auth = {"Authorization": f"Bearer {token}", "X-Filename": "bench.ino.bin"}
        assert c.post("/api/flash/2", content=b"x", headers=auth).status_code == 400  # a host, not a node
        assert c.post("/api/flash/16", content=b"", headers=auth).status_code == 400
        too_big = b"x" * (s.max_firmware_bytes + 1)
        assert c.post("/api/flash/16", content=too_big, headers=auth).status_code == 413
        r = c.post("/api/flash/16", content=b"\xe9" * 10, headers=auth)
        assert r.status_code == 200 and r.json()["ok"]
    assert started == [(16, 10, "bench.ino.bin")]


def test_bench_snapshot_carries_the_flash_status(tmp_path):
    from anv_dashboard.hub import Hub

    async def main():
        hub = Hub(settings(tmp_path), FakeBridge())
        hub.flash = FlashJobs(hub.settings, fake_tool([{"phase": "done", "pct": 100.0, "msg": "ok"}]))
        await hub.flash.start(18, b"x", "f.bin")
        await wait_finished(hub.flash)
        await hub.poll_once()

        class C:
            id, page, authed = "c1", "bench", True

        snap = hub.snapshot(C())
        assert snap["bench"]["flash"]["node_id"] == 18 and snap["bench"]["flash"]["state"] == "done"
        json.dumps(snap)  # must stay serialisable

    asyncio.run(main())
