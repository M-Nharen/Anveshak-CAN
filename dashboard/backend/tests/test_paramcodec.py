from anv_dashboard.paramcodec import to_physical, to_raw


def test_f32_round_trips_through_the_wire_bits():
    raw = to_raw("f32", 12.5)
    assert raw != 12  # not a numeric cast: it's the IEEE-754 bit pattern
    assert abs(to_physical("f32", raw) - 12.5) < 1e-6


def test_negative_f32():
    raw = to_raw("f32", -3.25)
    assert abs(to_physical("f32", raw) - (-3.25)) < 1e-6


def test_i32_round_trips_and_keeps_the_sign():
    raw = to_raw("i32", -1000)
    assert to_physical("i32", raw) == -1000.0


def test_u32_and_u8_are_plain_integers():
    assert to_raw("u32", 20000) == 20000
    assert to_physical("u32", 20000) == 20000.0
    assert to_raw("u8", 10) == 10
    assert to_physical("u8", 10) == 10.0
