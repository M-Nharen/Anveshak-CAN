"""Test bench: one motor at a time (docs/architecture.md "Control panel", part 4 / Pages table).
Jog is hold-to-run, same 200 ms rule as Drive; a step test holds a target for a fixed window
while the poll loop records target vs measured speed for the step-response plot.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field

from .settings import Settings

MODE_VELOCITY = 2


def clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


class JogManager:
    """Single-drive hold-to-run, independent of the main Drive page's ControlManager."""

    def __init__(self, settings: Settings, clock: Callable[[], float]) -> None:
        self._s = settings
        self._clock = clock
        self.holder: str | None = None
        self.drive: int | None = None
        self._speed = 0.0
        self._last_msg = 0.0
        self._moving = False

    def start(self, client: str, drive: int, speed: float) -> bool:
        if self.holder not in (None, client) or not (1 <= drive <= 6):
            return False
        self.holder = client
        self.drive = drive
        self._speed = clamp(float(speed), -self._s.max_wheel_speed, self._s.max_wheel_speed)
        self._last_msg = self._clock()
        return True

    def stop(self, client: str) -> bool:
        if self.holder != client:
            return False
        self._last_msg = 0.0
        return True

    def on_disconnect(self, client: str) -> None:
        if self.holder == client:
            self._last_msg = 0.0

    def tick(self) -> tuple[int, dict[str, float]] | None:
        """(drive, wheel target dict) to send, or None when there is nothing to do."""
        now = self._clock()
        if self.holder is not None and self.drive is not None and self._last_msg > 0 and (
            now - self._last_msg
        ) <= self._s.hold_timeout_s:
            self._moving = True
            return self.drive, {"mode": MODE_VELOCITY, "a": self._speed, "b": self._speed, "accel": self._s.default_accel}
        if self._moving:
            self._moving = False
            drive, self.drive = self.drive, None
            self.holder = None
            return drive, {"mode": MODE_VELOCITY, "a": 0.0, "b": 0.0, "accel": self._s.default_accel}
        return None


@dataclass
class StepTest:
    drive: int
    amplitude: float
    duration_s: float
    started: float
    samples: list[tuple[float, float, float]] = field(default_factory=list)  # (t, target, measured)

    def done(self, now: float) -> bool:
        return now - self.started >= self.duration_s

    def record(self, now: float, target: float, measured: float | None) -> None:
        if measured is not None:
            self.samples.append((round(now - self.started, 3), target, measured))
