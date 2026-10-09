from anv_dashboard.rates import RateEntry, catalog, check_rate_change
from anv_dashboard.settings import Settings


def test_catalog_uses_cached_node_values_and_falls_back_to_defaults():
    entries = catalog({"cycle_hz": 50.0, "host_hb_hz": 20.0}, {4: {"rate_fb_hz": 25.0}})
    by_id = {e.id: e for e in entries}
    assert by_id["cycle"].hz == 50.0
    assert by_id["host_hb"].hz == 20.0
    assert by_id["node:4:rate_fb_hz"].hz == 25.0  # cached
    assert by_id["node:4:rate_telem_hz"].hz == 10.0  # not cached: param default
    assert by_id["node:5:rate_fb_hz"].hz == 50.0  # drive 2 never cached: param default


def test_floors_cannot_be_overridden():
    entries = catalog({"cycle_hz": 50.0, "host_hb_hz": 20.0}, {})
    allowed, reason, _ = check_rate_change(entries, "cycle", 24.9, Settings())
    assert not allowed and "25" in reason
    allowed, _, _ = check_rate_change(entries, "cycle", 50.0, Settings())
    assert allowed
    allowed, reason, _ = check_rate_change(entries, "host_hb", 19.9, Settings())
    assert not allowed and "20" in reason
    allowed, reason, _ = check_rate_change(entries, "node:4:rate_hb_hz", 4.0, Settings())
    assert not allowed and "5" in reason  # tighter than the param's own min=1
    allowed, _, _ = check_rate_change(entries, "node:4:rate_hb_hz", 5.0, Settings())
    assert allowed


def test_ceiling_cannot_be_overridden():
    entries = catalog({"cycle_hz": 50.0, "host_hb_hz": 20.0}, {})
    allowed, reason, _ = check_rate_change(entries, "cycle", 50.1, Settings())
    assert not allowed and "50" in reason
    allowed, reason, _ = check_rate_change(entries, "node:4:rate_hb_hz", 10.1, Settings())
    assert not allowed


def test_load_guard_refuses_a_change_that_would_push_the_bus_over_50_percent():
    settings = Settings(can_bitrate=10_000, avg_frame_bits=100.0)
    # Two 1 kHz-capable entries, one already near the ceiling: baseline load is comfortably
    # under 50%, but pushing the second one the rest of the way tips it over.
    entries = [
        RateEntry("a", "A", "node", 4, hz=40.0, floor_hz=1.0, ceil_hz=1000.0, frames_per_occurrence=1),
        RateEntry("b", "B", "node", 5, hz=1.0, floor_hz=1.0, ceil_hz=1000.0, frames_per_occurrence=1),
    ]
    # baseline: (40*100 + 1*100) / 10000 = 41%
    allowed, reason, load = check_rate_change(entries, "b", 5.0, settings)
    assert allowed and load == 45.0
    allowed, reason, load = check_rate_change(entries, "b", 20.0, settings)
    assert not allowed and "50" in reason and load == 60.0


def test_unknown_rate_id_is_refused():
    entries = catalog({"cycle_hz": 50.0, "host_hb_hz": 20.0}, {})
    allowed, reason, _ = check_rate_change(entries, "no_such_rate", 10.0, Settings())
    assert not allowed and "no_such_rate" in reason
