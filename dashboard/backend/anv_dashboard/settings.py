"""Settings, read once from ANV_DASH_* environment variables."""

from __future__ import annotations

import os
import shutil
import tempfile
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Settings:
    gateway_socket: str = "/run/anveshak/gateway.sock"
    # Login password for anything that moves hardware. None = a random one is generated at start.
    password: str | None = None
    static_dir: Path | None = None  # the built frontend; None = API only
    # Treat browsers on this machine (the RPi's touch screen) as logged in. Off unless asked for.
    trust_localhost: bool = False
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

    # Firmware updates (anv-flash): the CAN interface the bus is on, where host/tools lives, and
    # where uploaded images wait while they are flashed.
    can_interface: str = "can1"
    tools_dir: Path = field(default_factory=lambda: Path(__file__).resolve().parents[3] / "host" / "tools")
    firmware_dir: Path = field(default_factory=lambda: Path(tempfile.gettempdir()) / "anv-firmware")
    max_firmware_bytes: int = 2 * 1024 * 1024
    # "gateway": the v2 bench gateway flashes (and .cpp uploads are built here); "anv_flash": the old
    # 11-bit tool. Building needs arduino-cli with the esp32 core.
    flash_mode: str = "gateway"
    repo_dir: Path = field(default_factory=lambda: Path(__file__).resolve().parents[3])
    arduino_cli: str = field(default_factory=lambda: shutil.which("arduino-cli")
                             or str(Path.home() / ".local" / "bin" / "arduino-cli"))
    build_cache_dir: Path = field(default_factory=lambda: Path.home() / ".cache" / "anveshak" / "builds")
    # Board (FQBN, with any menu options) to build a .cpp for, per chip: 1 = WROOM, 2 = ESP32-S3. An S3
    # board with only the native USB port wants "esp32:esp32:esp32s3:CDCOnBoot=cdc" so Serial shows there.
    fqbn_esp32: str = "esp32:esp32:esp32"
    fqbn_esp32s3: str = "esp32:esp32:esp32s3"

    can_bitrate: int = 500_000
    avg_frame_bits: float = (
        111.0  # standard 8-byte frame incl. stuffing, for the bus-load estimate
    )

    @classmethod
    def from_env(cls, env: dict[str, str] | None = None) -> Settings:
        e = os.environ if env is None else env
        s = cls()
        s.gateway_socket = e.get("ANV_DASH_GATEWAY_SOCKET", s.gateway_socket)
        s.password = e.get("ANV_DASH_PASSWORD") or None
        if e.get("ANV_DASH_STATIC"):
            s.static_dir = Path(e["ANV_DASH_STATIC"])
        s.trust_localhost = e.get("ANV_DASH_TRUST_LOCALHOST", "").lower() in ("1", "true", "yes", "on")
        s.host = e.get("ANV_DASH_HOST", s.host)
        s.can_interface = e.get("ANV_DASH_CAN_INTERFACE", s.can_interface)
        s.flash_mode = e.get("ANV_DASH_FLASH_MODE", s.flash_mode)
        s.arduino_cli = e.get("ANV_DASH_ARDUINO_CLI", s.arduino_cli)
        s.fqbn_esp32 = e.get("ANV_DASH_FQBN_ESP32", s.fqbn_esp32)
        s.fqbn_esp32s3 = e.get("ANV_DASH_FQBN_ESP32S3", s.fqbn_esp32s3)
        if e.get("ANV_DASH_BUILD_CACHE"):
            s.build_cache_dir = Path(e["ANV_DASH_BUILD_CACHE"])
        s.port = int(e.get("ANV_DASH_PORT", s.port))
        s.max_wheel_speed = float(e.get("ANV_DASH_MAX_WHEEL_SPEED", s.max_wheel_speed))
        s.default_cap = min(
            float(e.get("ANV_DASH_DEFAULT_CAP", s.default_cap)), s.max_wheel_speed
        )
        return s
