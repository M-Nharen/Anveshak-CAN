"""The Rates page: every periodic message, its current rate, allowed range, and the bus-load
guard (docs/architecture.md "Control panel", part 3). Pure functions, so the guard is tested
without a gateway: floors can't be overridden, and no change may push the projected load over 50%.
"""

from __future__ import annotations

from dataclasses import dataclass

from anv_can_defs.params_drive import PARAMS_DRIVE_BY_NAME

from .settings import Settings

BUS_LOAD_GUARD_PCT = 50.0

CYCLE_FLOOR_HZ, CYCLE_CEIL_HZ = 25.0, 50.0
HOST_HB_FLOOR_HZ, HOST_HB_CEIL_HZ = 20.0, 50.0
NODE_HB_FLOOR_HZ = 5.0  # tighter than rate_hb_hz's own min=1 (defs/params/drive.yaml)

DRIVE_NODE_IDS = tuple(range(4, 10))  # drive 1-6 = node ID 4-9

# (param name, frames sent per occurrence, floor_hz) for each node-sent rate.
NODE_RATE_PARAMS: tuple[tuple[str, int, float], ...] = (
    ("rate_fb_hz", 2, 1.0),  # DRIVE_FB_A + DRIVE_FB_B
    ("rate_telem_hz", 2, 1.0),  # DRIVE_TELEM_0 + _1
    ("rate_hb_hz", 1, NODE_HB_FLOOR_HZ),  # NODE_HB
)


@dataclass
class RateEntry:
    id: str  # "cycle" | "host_hb" | "node:<id>:<param name>"
    label: str
    source: str  # "host" | "node"
    node_id: int | None
    hz: float  # current value
    floor_hz: float
    ceil_hz: float
    frames_per_occurrence: int  # how many frames one "occurrence" at this rate puts on the bus


def catalog(state: dict, node_rates: dict[int, dict[str, int]]) -> list[RateEntry]:
    """Every periodic message the Rates page can change, built from the gateway state (host
    rates) and the cached node parameter values (node rates; see Hub.refresh_rates)."""
    entries = [
        RateEntry("cycle", "Control cycle (SYNC + DRIVE_CMD)", "host", None, state.get("cycle_hz", 50.0),
                   CYCLE_FLOOR_HZ, CYCLE_CEIL_HZ, 1 + 6),
        RateEntry("host_hb", "This host's heartbeat", "host", None, state.get("host_hb_hz", 20.0),
                   HOST_HB_FLOOR_HZ, HOST_HB_CEIL_HZ, 1),
    ]  # fmt: skip
    for node_id in DRIVE_NODE_IDS:
        cached = node_rates.get(node_id, {})
        for name, frames, floor_hz in NODE_RATE_PARAMS:
            info = PARAMS_DRIVE_BY_NAME[name]
            hz = float(cached.get(name, info.default))
            entries.append(
                RateEntry(
                    f"node:{node_id}:{name}",
                    f"Drive {node_id - 3} {name}",
                    "node",
                    node_id,
                    hz,
                    max(floor_hz, info.minimum),
                    info.maximum,
                    frames,
                )
            )
    return entries


def floor_reason(entry: RateEntry, new_hz: float) -> str | None:
    """None if `new_hz` is within the entry's allowed range, else why not."""
    if new_hz < entry.floor_hz:
        return f"{entry.label} cannot go below {entry.floor_hz:g} Hz"
    if new_hz > entry.ceil_hz:
        return f"{entry.label} cannot go above {entry.ceil_hz:g} Hz"
    return None


def projected_load_pct(entries: list[RateEntry], changed_id: str, new_hz: float, settings: Settings) -> float:
    """Bus load estimated purely from the catalog, if `changed_id` moved to `new_hz` and nothing
    else changed. The DBC-rate estimate the Rates page shows while a selector moves."""
    bits_per_s = 0.0
    for e in entries:
        hz = new_hz if e.id == changed_id else e.hz
        bits_per_s += hz * e.frames_per_occurrence * settings.avg_frame_bits
    return bits_per_s / settings.can_bitrate * 100.0


def measured_load_pct(state: dict, entry: RateEntry, new_hz: float, settings: Settings) -> float:
    """The gateway's actually-measured bus load (which already reflects traffic the catalog
    does not model: FPGA sensors, debug pages, SERVICE, events...) plus the delta this one
    change would add. More realistic than the catalog estimate alone, and what makes the guard
    reachable even when the catalog's own worst case is well under 50% (docs/architecture.md's
    own "about 4.9%" per fully-loaded drive node)."""
    base_bps = state.get("bus", {}).get("rx_fps", 0.0) * settings.avg_frame_bits
    delta_bps = (new_hz - entry.hz) * entry.frames_per_occurrence * settings.avg_frame_bits
    return (base_bps + delta_bps) / settings.can_bitrate * 100.0


def check_rate_change(
    entries: list[RateEntry], changed_id: str, new_hz: float, settings: Settings, state: dict | None = None
) -> tuple[bool, str, float]:
    """(allowed, reason_if_refused, projected_load_pct)."""
    entry = next((e for e in entries if e.id == changed_id), None)
    if entry is None:
        return False, f"unknown rate '{changed_id}'", 0.0
    reason = floor_reason(entry, new_hz)
    load = projected_load_pct(entries, changed_id, new_hz, settings)
    if state is not None:
        load = max(load, measured_load_pct(state, entry, new_hz, settings))
    if reason:
        return False, reason, load
    if load > BUS_LOAD_GUARD_PCT:
        return False, f"would push the bus to {load:.1f}% (over {BUS_LOAD_GUARD_PCT:g}%)", load
    return True, "", load
