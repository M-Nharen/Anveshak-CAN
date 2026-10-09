"""Manual drive control: the control lock and hold-to-run.

Pure logic with an injected clock, so the 200 ms rule is tested without sleeping.

* Only one browser (client id) holds control. It is released if that browser disconnects.
* The holder sends drive messages (forward, turn in -1..1; positive turn = clockwise, i.e. to the right) at about 20 Hz while a key or the
  joystick is held. If nothing arrives for `hold_timeout_s`, the targets are zero.
* `tick()` is called at control_hz; it returns the six wheel targets to send to the gateway, or
  None when there is nothing to send. After the last non-zero command it returns one zero set.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from .settings import Settings

MODE_VELOCITY = 2
NUM_DRIVES = 6


def clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


@dataclass
class Limits:
    cap: float  # rad/s: no wheel is commanded faster than cap * level
    accel: float  # rad/s^2
    level: float = 1.0  # 0.1 .. 1.0, moved by the speed +/- buttons


class ControlManager:
    def __init__(self, settings: Settings, clock: Callable[[], float]) -> None:
        self._s = settings
        self._clock = clock
        self.holder: str | None = None
        self.limits = Limits(cap=settings.default_cap, accel=settings.default_accel)
        self._fwd = 0.0
        self._turn = 0.0
        self._last_msg = 0.0
        self._moving = False  # non-zero targets were sent and no zero has followed yet
        self.last_targets: list[float] = [
            0.0
        ] * NUM_DRIVES  # per drive, rad/s (both channels)

    # --- the lock -------------------------------------------------------------------------------
    def take(self, client: str) -> bool:
        if self.holder not in (None, client):
            return False
        self.holder = client
        self._stop_intent()
        return True

    def release(self, client: str) -> bool:
        if self.holder != client:
            return False
        self.holder = None
        self._stop_intent()
        return True

    def halt(self) -> None:
        """Forget the current drive input (E-stop): the next tick sends zero."""
        self._stop_intent()

    def on_disconnect(self, client: str) -> None:
        self.release(client)

    # --- drive input ----------------------------------------------------------------------------
    def drive(self, client: str, fwd: float, turn: float) -> bool:
        """Latest joystick/key state from the holder. False if `client` does not hold control."""
        if self.holder != client:
            return False
        self._fwd = clamp(float(fwd), -1.0, 1.0)
        self._turn = clamp(float(turn), -1.0, 1.0)
        self._last_msg = self._clock()
        return True

    def step_speed(self, client: str, direction: int) -> bool:
        if self.holder != client:
            return False
        self.limits.level = clamp(
            round(self.limits.level + direction * self._s.speed_step, 3),
            self._s.speed_step,
            1.0,
        )
        return True

    def set_limits(self, client: str, cap: float | None, accel: float | None) -> bool:
        if self.holder != client:
            return False
        if cap is not None:
            self.limits.cap = clamp(float(cap), 0.0, self._s.max_wheel_speed)
        if accel is not None:
            self.limits.accel = clamp(float(accel), 0.1, self._s.max_accel)
        return True

    # --- output ---------------------------------------------------------------------------------
    def tick(self) -> list[dict[str, float]] | None:
        now = self._clock()
        if (
            self.holder is not None
            and (now - self._last_msg) <= self._s.hold_timeout_s
            and self._last_msg > 0
        ):
            return self._targets(self._fwd, self._turn)
        # Not held, or nothing heard for 200 ms: zero, once.
        if self._moving:
            return self._targets(0.0, 0.0)
        return None

    def _targets(self, fwd: float, turn: float) -> list[dict[str, float]]:
        top = self.limits.cap * self.limits.level
        left = clamp(fwd + turn, -1.0, 1.0) * top
        right = clamp(fwd - turn, -1.0, 1.0) * top
        wheels = []
        per_drive = []
        for drive in range(1, NUM_DRIVES + 1):
            if drive in self._s.left_drives:
                v = left
            elif drive in self._s.right_drives:
                v = right
            else:
                v = 0.0
            per_drive.append(v)
            wheels.append(
                {"mode": MODE_VELOCITY, "a": v, "b": v, "accel": self.limits.accel}
            )
        self.last_targets = per_drive
        self._moving = any(v != 0.0 for v in per_drive)
        return wheels

    def _stop_intent(self) -> None:
        self._fwd = self._turn = 0.0
        self._last_msg = 0.0
