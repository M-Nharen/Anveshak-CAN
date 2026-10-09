"""anv-flash against fake nodes on a virtual bus: list, flash --node, flash --type, rescue,
and every refusal in docs/architecture.md's "Safety rules for flashing"."""

from __future__ import annotations

import pytest

from anv_flash import cli
from anv_flash.bus import STATE_ARMED, STATE_BOOTLOADER, STATE_DISARMED
from anv_flash.flasher import FlashError, SafetyRefusal
from anv_flash.image import FirmwareImage
from conftest import make_image

OLD = make_image(build_id=0x0000AAAA, seed=1)
NEW = make_image(build_id=0x5EED0B0B, size=12000, seed=2)


def _image(data: bytes) -> FirmwareImage:
    return FirmwareImage.from_bytes(data)


def test_list_shows_nodes_and_bootloader_info(rig):
    rig.node(4, OLD)
    rig.node(6, None, start_in_bootloader=True)
    rows = rig.flasher().list_nodes()
    by_id = {r.status.node_id: r for r in rows}
    assert set(by_id) == {4, 6}
    assert by_id[4].status.state == STATE_DISARMED
    assert by_id[4].status.build_id16 == 0xAAAA
    assert by_id[4].info is None
    assert by_id[6].status.state == STATE_BOOTLOADER
    assert by_id[6].info is not None and by_id[6].info.boot_status != 0  # no valid app


def test_flash_node(rig):
    node = rig.node(4, OLD)
    rig.flasher().flash_node(4, _image(NEW))
    assert bytes(node.flash.mem[: len(NEW)]) == NEW
    assert node.mode == "app"
    assert node.build16 == 0x0B0B
    assert node.history == ["bootloader", "app", "bootloader", "bootloader", "app"]
    assert any("running build 0x0B0B, OK" in line for line in rig.log)


def test_flash_node_already_in_bootloader(rig):
    node = rig.node(5, None, start_in_bootloader=True)
    rig.flasher().flash_node(5, _image(NEW))
    assert node.mode == "app" and node.build16 == 0x0B0B


def test_flash_type_one_at_a_time(rig):
    a = rig.node(4, OLD)
    b = rig.node(5, OLD)
    other = rig.node(20, make_image(node_type=4, build_id=0xF0), node_type=4)  # FPGA
    flasher = rig.flasher()
    assert flasher.flash_type(1, _image(NEW)) == [4, 5]
    assert a.build16 == b.build16 == 0x0B0B
    assert other.history == ["bootloader", "app"]  # untouched
    # One at a time: node 5 was still running its application while node 4 was flashed.
    first_done = next(i for i, line in enumerate(rig.log) if "node 4: running build" in line)
    assert not any("node 5:" in line for line in rig.log[:first_done])


def test_refuses_when_a_node_is_armed(rig):
    rig.node(4, OLD)
    busy = rig.node(5, OLD, state=STATE_ARMED)
    with pytest.raises(SafetyRefusal, match="node 5 ARMED"):
        rig.flasher().flash_node(4, _image(NEW))
    assert busy.history == ["bootloader", "app"]
    assert rig.nodes[0].history == ["bootloader", "app"]  # nothing sent to node 4 either


def test_refuses_unless_this_host_leads(rig):
    rig.node(4, OLD)
    rig.host.role = 1  # standby
    with pytest.raises(SafetyRefusal, match="not the master"):
        rig.flasher().flash_node(4, _image(NEW))
    with pytest.raises(SafetyRefusal, match="identity"):
        rig.flasher(host_id=None).flash_node(4, _image(NEW))
    with pytest.raises(SafetyRefusal, match="no heartbeat from this host"):
        rig.flasher(host_id=1).flash_node(4, _image(NEW))


def test_refuses_wrong_node_type(rig):
    node = rig.node(4, OLD)
    arm_image = _image(make_image(node_type=2))
    with pytest.raises(SafetyRefusal, match="drive node"):
        rig.flasher().flash_node(4, arm_image)
    with pytest.raises(SafetyRefusal, match="node type"):
        rig.flasher().flash_type(1, arm_image)
    assert node.history == ["bootloader", "app"]


def test_build_id_checked_after_boot(rig):
    rig.node(4, OLD, report_build=0x7777)  # boots, but reports some other build
    with pytest.raises(FlashError, match="expected 0x0B0B"):
        rig.flasher().flash_node(4, _image(NEW))


def test_flash_error_aborts_and_stays_in_bootloader(rig):
    node = rig.node(4, OLD)
    node.flash.fail_writes_after = 2
    with pytest.raises(FlashError, match="FLASH_ERROR.*FLASH_WRITE_FAIL.*partial image erased"):
        rig.flasher().flash_node(4, _image(NEW))
    assert node.mode == "bootloader"
    assert node.flash.app_check(1) != 0
    assert all(b == 0xFF for b in node.flash.mem[: len(NEW)])


def test_silent_node_caught_in_boot_window(rig):
    # Crash-looping application: never sends a heartbeat, only its bootloader window is open.
    node = rig.node(4, OLD, crash_build=0x0000AAAA)
    rig.flasher().flash_node(4, _image(NEW))
    assert node.mode == "app" and node.build16 == 0x0B0B


def test_rescue_by_uid(rig):
    stuck = rig.node(7, OLD, crash_build=0x0000AAAA, uid=b"\x11" * 12)
    spare = rig.node(31, None, start_in_bootloader=True, uid=b"\x22" * 12)  # unconfigured board
    flasher = rig.flasher()
    assert flasher.rescue(stuck.logic.uid32, _image(NEW)) == 7
    assert stuck.mode == "app" and stuck.build16 == 0x0B0B
    assert spare.mode == "bootloader" and spare.flash.app_check(1) != 0

    assert flasher.rescue(spare.logic.uid32, _image(NEW)) == 31
    assert spare.mode == "app" and spare.build16 == 0x0B0B


def test_flash_node_31_needs_rescue(rig):
    with pytest.raises(SafetyRefusal, match="rescue"):
        rig.flasher().flash_node(31, _image(NEW))


def test_cli(rig, tmp_path, capsys):
    rig.node(4, OLD)
    path = tmp_path / "drive.bin"
    path.write_bytes(NEW)
    argv = ["--host", "tool", "flash", "--node", "4", str(path)]
    assert cli.main(argv, bus=rig.tool_bus) == cli.EXIT_OK
    out = capsys.readouterr().out
    assert "running build 0x0B0B" in out and out.strip().endswith("done")

    rig.host.role = 1
    assert cli.main(argv, bus=rig.tool_bus) == cli.EXIT_REFUSED

    assert cli.main(["list"], bus=rig.tool_bus) == cli.EXIT_OK
    assert "drive" in capsys.readouterr().out

    bad = tmp_path / "bad.bin"
    bad.write_bytes(NEW[:-1])
    assert cli.main(["--host", "tool", "flash", "--node", "4", str(bad)], bus=rig.tool_bus) == cli.EXIT_FAILED
