"""Settings, read once from ANV_DASH_* environment variables."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Settings:
    gateway_socket: str = "/run/anveshak/gateway.sock"
    # Login password for anything that moves hardware. None = a random one is generated at start.
    password: str | None = None
    static_dir: Path | None = None  # the built frontend; None = API only
    host: str = "0.0.0.0"
    port: int = 8080

    poll_hz: float = 20.0  # how often the gateway state is fetched
    push_hz: float = 10.0  # updates per second to a browser (drive page: drive_push_hz)
    drive_push_hz: float = 20.0
    control_hz: float = (
        50.0  # target refresh towards the gateway while a browser drives
    )
    hold_timeout_s: float = 0.2  # no drive message for this long -> zero speed
    session_ttl_s: float = 8 * 3600.0

    # Drive limits (rad/s at the wheel). The cap slider moves between 0 and max_wheel_speed.
    max_wheel_speed: float = 6.0
    default_cap: float = 3.0
    max_accel: float = 20.0
    default_accel: float = 5.0
    speed_step: float = 0.1  # fraction of the cap per "speed +/-" press

    # Which drive nodes (1-6) are on each side of the rover. An assumption, same as the ROS package.
    left_drives: tuple[int, ...] = (1, 2, 3)
    right_drives: tuple[int, ...] = (4, 5, 6)

    can_bitrate: int = 500_000
    avg_frame_bits: float = (
        111.0  # standard 8-byte frame incl. stuffing, for the bus-load estimate
    )

    max_cached_events: int = 1000  # events kept in memory for the Errors page

    @classmethod
    def from_env(cls, env: dict[str, str] | None = None) -> Settings:
        e = os.environ if env is None else env
        s = cls()
        s.gateway_socket = e.get("ANV_DASH_GATEWAY_SOCKET", s.gateway_socket)
        s.password = e.get("ANV_DASH_PASSWORD") or None
        if e.get("ANV_DASH_STATIC"):
            s.static_dir = Path(e["ANV_DASH_STATIC"])
        s.host = e.get("ANV_DASH_HOST", s.host)
        s.port = int(e.get("ANV_DASH_PORT", s.port))
        s.max_wheel_speed = float(e.get("ANV_DASH_MAX_WHEEL_SPEED", s.max_wheel_speed))
        s.default_cap = min(
            float(e.get("ANV_DASH_DEFAULT_CAP", s.default_cap)), s.max_wheel_speed
        )
        return s
