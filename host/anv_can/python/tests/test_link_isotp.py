from __future__ import annotations

import threading

import pytest

import anv_can


@pytest.fixture(scope="module")
def schema():
    return anv_can.load_schema()


def test_test_bus_delivers_and_filters():
    bus = anv_can.TestBus()
    a, b = bus.connect(), bus.connect()
    assert a.send(anv_can.Frame(0x1234567, b"\x01\x02"))
    f = b.recv(100)
    assert f.id == 0x1234567 and f.data == b"\x01\x02" and f.extended
    assert a.recv(0) is None  # no echo
    b.set_filters([(20 << 18, 0xFF << 18)])
    a.send(anv_can.Frame(21 << 18))
    a.send(anv_can.Frame(20 << 18 | 3))
    assert b.recv(50).id == 20 << 18 | 3
    assert b.recv(10) is None
    assert a.stats["tx_frames"] == 3
    b.close()
    with pytest.raises(EOFError):
        b.recv(10)


def test_frame_text():
    f = anv_can.Frame.parse("04504001#02960038FF0000")
    assert f.id == 0x04504001 and f.data == bytes.fromhex("02960038FF0000")
    assert str(f) == "04504001#02960038FF0000"
    with pytest.raises(ValueError):
        anv_can.Frame(1, b"123456789")


@pytest.mark.parametrize("size", [1, 7, 8, 100, 4095, 20000])
def test_isotp_round_trip(schema, size):
    bus = anv_can.TestBus()
    host = anv_can.IsoTpChannel(bus.connect(), schema, local=1, peer=20)
    node = anv_can.IsoTpChannel(bus.connect(), schema, local=20, peer=1, block_size=8)
    payload = bytes((i * 13) & 0xFF for i in range(size))
    got: list[bytes] = []
    th = threading.Thread(target=lambda: got.append(node.recv(3000)))
    th.start()
    host.send(payload, 3000)
    th.join()
    assert got == [payload]
    p = schema.parse_id(host.tx_id)
    assert (p["src"], p["dst"], p["port"]) == (1, 20, schema.protocol.message("transfer").port)


def test_isotp_lost_frame(schema):
    bus = anv_can.TestBus()
    seen = {"cf": 0}

    def tap(frame):
        if frame.data[0] >> 4 == 2:
            seen["cf"] += 1
            return seen["cf"] != 3
        return True

    bus.set_tap(tap)
    host = anv_can.IsoTpChannel(bus.connect(), schema, local=1, peer=20)
    node = anv_can.IsoTpChannel(bus.connect(), schema, local=20, peer=1)
    errors: list[str] = []

    def rx():
        try:
            node.recv(3000)
        except anv_can.IsoTpError as e:
            errors.append(str(e))

    th = threading.Thread(target=rx)
    th.start()
    host.send(bytes(100), 3000)
    th.join()
    assert errors == ["lost frame"]


def test_isotp_timeout(schema):
    bus = anv_can.TestBus()
    host = anv_can.IsoTpChannel(bus.connect(), schema, local=1, peer=20, timeout_ms=20)
    with pytest.raises(anv_can.IsoTpError, match="timeout"):
        host.send(bytes(50), 1000)
