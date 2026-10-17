import itertools
import json
import sys
from pathlib import Path

import can
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from anv_flash.cli import check_allowed, wait_for_new_image
from anv_flash.image import ImageError, parse_image
from anv_flash.isotp import stmin_seconds
from anv_flash.protocol import FlashError, Updater

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fake_node import FakeNode, make_image

_channels = itertools.count()


@pytest.fixture
def channel():
    return f"flash-{next(_channels)}"


@pytest.fixture
def host(channel):
    bus = can.Bus(interface="virtual", channel=channel, receive_own_messages=False)
    yield bus
    bus.shutdown()


def node_on(channel, **kw):
    return FakeNode(channel, **kw)


def test_full_flash_and_the_node_comes_back_with_the_new_build(channel, host):
    node = node_on(channel)
    try:
        image = parse_image(make_image(build_id=0xBEEF))
        phases = []
        Updater(host, 16).flash(image, lambda phase, pct: phases.append(phase))
        assert bytes(node.flash) == image.data and node.committed
        assert phases[0] == "check" and "write" in phases and phases[-1] == "boot"
        assert "running the new image" in wait_for_new_image(host, 16, 0xBEEF, 0x1111)
    finally:
        node.close()


def test_wrong_chip_is_refused_before_anything_is_written(channel, host):
    node = node_on(channel, hw_target=1)  # an S3
    try:
        with pytest.raises(FlashError, match="ESP32-S3"):
            Updater(host, 16).flash(parse_image(make_image(hw_target=0)))
        assert node.writes == 0
    finally:
        node.close()


def test_corrupted_transfer_fails_verify_and_aborts(channel, host):
    node = node_on(channel, corrupt=True)
    try:
        with pytest.raises(FlashError, match="CRC_MISMATCH"):
            Updater(host, 16).flash(parse_image(make_image()))
        assert not node.committed and node.aborted
    finally:
        node.close()


def test_a_lost_frame_is_recovered_by_resending_the_block(channel, host):
    node = node_on(channel, lose_cf_once=True)
    try:
        image = parse_image(make_image())
        Updater(host, 16, reply_timeout=0.5).flash(image)
        assert bytes(node.flash) == image.data
    finally:
        node.close()


def test_armed_node_refuses(channel, host):
    node = node_on(channel, armed=True)
    try:
        with pytest.raises(FlashError, match="armed"):
            Updater(host, 16).flash(parse_image(make_image()))
    finally:
        node.close()


def test_silent_node_is_reported(channel, host):
    node = node_on(channel, silent=True)
    try:
        with pytest.raises(FlashError, match="doesn't answer"):
            Updater(host, 16, reply_timeout=0.3).flash(parse_image(make_image()))
    finally:
        node.close()


def test_rollback_is_detected_after_boot(channel, host):
    node = node_on(channel, rollback=True)
    try:
        Updater(host, 16).flash(parse_image(make_image(build_id=0xBEEF)))
        with pytest.raises(FlashError, match="rolled back"):
            wait_for_new_image(host, 16, 0xBEEF, 0x1111)
    finally:
        node.close()


def test_image_checks():
    img = parse_image(make_image(hw_target=1, build_id=0x16BB))
    assert img.hw_target == 1 and img.build_id == 0x16BB and img.project == "bench_esp32_arduino"
    merged = b"\xff" * 0x10000 + make_image()
    with pytest.raises(ImageError, match="merged"):
        parse_image(merged)
    with pytest.raises(ImageError, match="too small"):
        parse_image(b"\xe9" * 10)
    bad_chip = bytearray(make_image())
    bad_chip[12] = 5  # ESP32-C3
    with pytest.raises(ImageError, match="chip id 5"):
        parse_image(bytes(bad_chip))


def test_stmin_codes():
    assert stmin_seconds(0) == 0 and stmin_seconds(5) == 0.005
    assert stmin_seconds(0xF5) == pytest.approx(0.0005)
    assert stmin_seconds(0x90) == 0.127


def test_bench_rule_for_who_may_flash():
    def state(role=1, allocator=True, alive=True, node_state=1):
        return {"role": role, "id_allocator": allocator,
                "nodes": [{"node_id": 16, "alive": alive, "state": node_state}]}  # fmt: skip

    check_allowed(state(), 16)  # no leader, this host hands out IDs
    check_allowed(state(role=2, allocator=False), 16)  # this host leads
    with pytest.raises(FlashError, match="another host"):
        check_allowed(state(allocator=False), 16)
    with pytest.raises(FlashError, match="not on the bus"):
        check_allowed(state(alive=False), 16)
    with pytest.raises(FlashError, match="armed"):
        check_allowed(state(node_state=2), 16)


def test_cli_flash_prints_json_progress_ending_in_done(channel, host, tmp_path, capsys):
    import argparse

    from anv_flash.cli import Reporter, run

    node = node_on(channel)
    try:
        path = tmp_path / "bench_esp32_arduino.ino.bin"
        path.write_bytes(make_image(build_id=0xCAFE))
        args = argparse.Namespace(cmd="flash", node=16, file=str(path), json=True, no_gateway_check=True,
                                  bridge="/nonexistent", channel=channel, interface="virtual")  # fmt: skip
        run(args, host, Reporter(True))
        events = [json.loads(line) for line in capsys.readouterr().out.splitlines()]
        phases = [e["phase"] for e in events]
        assert phases[0] == "check" and "write" in phases and phases[-1] == "done"
        assert "cafe" in events[-1]["msg"]
    finally:
        node.close()
