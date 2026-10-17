"""A CAN-bus simulator that pretends to be the rover's 6 drive nodes and the FPGA, and optionally
auto-addressed ESP32 bench nodes.

Lets both the firmware and host sides develop and test against something that behaves
like the real bus (docs/architecture.md "Node firmware", "Bring-up plan") without any
hardware. Talks python-can + cantools against defs/rover.dbc, so it stays correct as the
DBC changes.
"""
from __future__ import annotations

import math
import random
import time
from dataclasses import dataclass, field
from pathlib import Path

import can
import cantools

REPO_ROOT = Path(__file__).resolve().parents[2]
DBC_PATH = REPO_ROOT / "defs" / "rover.dbc"

CONTROL_CYCLE_S = 0.020          # SYNC every 20 ms (50 Hz)
CMD_TIMEOUT_S = 0.100            # command watchdog, 5 missed cycles
HEARTBEAT_PERIOD_S = 0.100       # NODE_HB, 10 Hz
TELEMETRY_PERIOD_S = 0.100       # DRIVE_TELEM_0/1, 10 Hz
SENSOR_PERIOD_S = 0.020          # FPGA_SENSORS_A/B, 50 Hz
ENCODER_COUNTS_PER_REV = 4096
RAD_PER_REV = 2 * math.pi
COUNTS_PER_RAD = ENCODER_COUNTS_PER_REV / RAD_PER_REV

STATE_BOOT = 0
STATE_DISARMED = 1
STATE_ARMED = 2
STATE_FAULT = 3
STATE_ESTOP = 4
STATE_BOOTLOADER = 5

NMT_ARM = 1
NMT_DISARM = 2
NMT_RESET = 3
NMT_ENTER_BOOTLOADER = 4
NMT_FAULT_RESET = 5

# Automatic node IDs (docs/architecture.md "Automatic node IDs")
NODE_ID_REQUEST_ID = 0x5FB
NODE_ID_ASSIGN_IDS = (0x5F8, 0x5F9, 0x5FA)  # from Orin, RPi, tool
HW_TARGET_ESP32 = 0
HW_TARGET_ESP32S3 = 1
ESP_SENSOR_PERIOD_S = 0.100  # bench nodes send their 4 values at 10 Hz

FAULT_OVERCURRENT_A = 1 << 0
FAULT_OVERCURRENT_B = 1 << 1


def load_db() -> cantools.database.can.Database:
    return cantools.database.load_file(DBC_PATH)


@dataclass
class DriveNodeSim:
    """One simulated drive node (STM32), node IDs 4-9."""

    drive_num: int  # 1-6
    db: cantools.database.can.Database
    boot_time: float = field(default_factory=time.monotonic)

    state: int = STATE_DISARMED
    current_master: tuple | None = None  # (master_node_id, term)
    last_cmd_time: float = 0.0
    fault_flags: int = 0
    dropped: bool = False

    pending_a: int = 0
    pending_b: int = 0
    measured_a: int = 0
    measured_b: int = 0
    encoder_a: int = 0
    encoder_b: int = 0

    next_hb_time: float = 0.0
    next_telem_time: float = 0.0
    commands_missed: int = 0
    hb_counter: int = 0

    def __post_init__(self) -> None:
        self.node_id = 3 + self.drive_num
        self.name = f"DRIVE{self.drive_num}"
        off = (self.drive_num - 1) * 8
        self.id_cmd = 0x120 + off
        self.id_fb_a = 0x220 + off
        self.id_fb_b = 0x221 + off
        self.id_telem_0 = 0x320 + off
        self.id_telem_1 = 0x321 + off
        self.id_node_hb = 0x620 + off

    # -- bus-event handlers -------------------------------------------------

    def on_estop(self, now: float) -> None:
        self.state = STATE_ESTOP
        self.fault_flags |= 1 << 7
        self.measured_a = self.measured_b = 0

    def on_estop_release(self, now: float) -> None:
        if self.state == STATE_ESTOP:
            self.fault_flags &= ~(1 << 7)
            self.state = STATE_DISARMED

    def on_nmt(self, decoded: dict, now: float) -> None:
        target = decoded["TARGET_NODE"]
        if target not in (0, self.node_id):
            return
        command = decoded["COMMAND"]
        master = (decoded["MASTER_NODE_ID"], decoded["MASTER_TERM"])
        if command == NMT_ARM:
            if self.state == STATE_DISARMED:
                self.current_master = master
                self.state = STATE_ARMED
                self.last_cmd_time = now
        elif command == NMT_DISARM:
            if self.state == STATE_ARMED:
                self.state = STATE_DISARMED
        elif command == NMT_RESET:
            self.state = STATE_DISARMED
            self.current_master = None
        elif command == NMT_ENTER_BOOTLOADER:
            self.state = STATE_BOOTLOADER
        elif command == NMT_FAULT_RESET and self.state == STATE_FAULT and self.fault_flags == 0:
            self.state = STATE_DISARMED

    def on_drive_cmd(self, decoded: dict, now: float) -> None:
        if self.state != STATE_ARMED or self.current_master is None:
            return
        master_id, term = self.current_master
        if decoded["SENDER_HOST"] != master_id or decoded["TERM_LOW"] != (term & 0x3):
            # A different master or a new term took over without a fresh ARM: disarm.
            self.state = STATE_DISARMED
            self.current_master = None
            return
        self.pending_a = decoded["TARGET_A"]
        self.pending_b = decoded["TARGET_B"]
        self.last_cmd_time = now

    def on_sync(self, now: float, bus: can.BusABC) -> None:
        if self.dropped:
            return
        if self.state == STATE_ARMED:
            self.measured_a = self.pending_a
            self.measured_b = self.pending_b
        else:
            self.measured_a = self.measured_b = 0

        dt = CONTROL_CYCLE_S
        self.encoder_a += round(self.measured_a * 0.01 * dt * COUNTS_PER_RAD)
        self.encoder_b += round(self.measured_b * 0.01 * dt * COUNTS_PER_RAD)

        self._send_feedback(bus)

    # -- periodic work --------------------------------------------------

    def tick(self, now: float, bus: can.BusABC) -> None:
        if self.dropped:
            return

        if self.state == STATE_ARMED and (now - self.last_cmd_time) > CMD_TIMEOUT_S:
            self.state = STATE_DISARMED
            self.current_master = None
            self.commands_missed += 1
            self.measured_a = self.measured_b = 0

        if now >= self.next_hb_time:
            self._send_heartbeat(bus)
            self.next_hb_time = now + HEARTBEAT_PERIOD_S

        if now >= self.next_telem_time:
            self._send_telemetry(bus)
            self.next_telem_time = now + TELEMETRY_PERIOD_S

    # -- fault injection --------------------------------------------------

    def inject_overcurrent(self) -> None:
        self.fault_flags |= FAULT_OVERCURRENT_A
        self.state = STATE_FAULT
        self.measured_a = self.measured_b = 0

    def drop(self) -> None:
        self.dropped = True

    # -- frame senders --------------------------------------------------

    def _send(self, bus: can.BusABC, name: str, data: dict) -> None:
        msg = self.db.get_message_by_name(name)
        frame = can.Message(arbitration_id=msg.frame_id, is_extended_id=False,
                             data=msg.encode(data, scaling=False))
        bus.send(frame)

    def _send_feedback(self, bus: can.BusABC) -> None:
        self._send(bus, f"{self.name}_FB_A", {
            "STATE": self.state,
            "FAULT_FLAGS": self.fault_flags,
            "SPEED_A": self.measured_a,
            "SPEED_B": self.measured_b,
            "CURRENT_A": 0,
            "CURRENT_B": 0,
        })
        self._send(bus, f"{self.name}_FB_B", {
            "ENCODER_A": self.encoder_a,
            "ENCODER_B": self.encoder_b,
        })

    def _send_heartbeat(self, bus: can.BusABC) -> None:
        uptime = int(time.monotonic() - self.boot_time)
        self._send(bus, f"{self.name}_NODE_HB", {
            "STATE": self.state,
            "NODE_TYPE": 1,
            "PROTOCOL_VERSION": 1,
            "CAN_ERROR_STATE": 0,
            "BOOTLOADER_RUNNING": 1 if self.state == STATE_BOOTLOADER else 0,
            "UNSAVED_PARAM_CHANGES": 0,
            "DEBUG_LEVEL": 0,
            "FW_BUILD_ID": 0,
            "UPTIME_S": min(uptime, 65535),
        })
        self.hb_counter += 1

    def _send_telemetry(self, bus: can.BusABC) -> None:
        self._send(bus, f"{self.name}_TELEM_0", {
            "SUPPLY_VOLTAGE": 24000,
            "TEMP_DRIVER_A": 25,
            "TEMP_DRIVER_B": 25,
            "TEMP_MCU": 30,
            "TEC": 0,
            "REC": 0,
            "BUS_OFF_RECOVERY_COUNT": 0,
        })
        self._send(bus, f"{self.name}_TELEM_1", {
            "DUTY_A": 0,
            "DUTY_B": 0,
            "WATCHDOG_RESET_COUNT": 0,
            "LAST_RESET_CAUSE": 0,
            "COMMANDS_MISSED": min(self.commands_missed, 65535),
        })


@dataclass
class FpgaNodeSim:
    """The FPGA sensor node (node ID 20): a passive sensor stream, no commands to follow."""

    db: cantools.database.can.Database
    boot_time: float = field(default_factory=time.monotonic)
    next_sensor_time: float = 0.0
    next_hb_time: float = 0.0
    dropped: bool = False

    def drop(self) -> None:
        self.dropped = True

    def tick(self, now: float, bus: can.BusABC) -> None:
        if self.dropped:
            return
        if now >= self.next_sensor_time:
            msg_a = self.db.get_message_by_name("FPGA_SENSORS_A")
            bus.send(can.Message(arbitration_id=msg_a.frame_id, is_extended_id=False,
                                  data=msg_a.encode({f"SENSOR_{i}": 0 for i in range(1, 5)}, scaling=False)))
            msg_b = self.db.get_message_by_name("FPGA_SENSORS_B")
            bus.send(can.Message(arbitration_id=msg_b.frame_id, is_extended_id=False,
                                  data=msg_b.encode({f"SENSOR_{i}": 0 for i in range(5, 9)}, scaling=False)))
            self.next_sensor_time = now + SENSOR_PERIOD_S
        if now >= self.next_hb_time:
            uptime = int(time.monotonic() - self.boot_time)
            msg = self.db.get_message_by_name("FPGA_NODE_HB")
            bus.send(can.Message(arbitration_id=msg.frame_id, is_extended_id=False, data=msg.encode({
                "STATE": STATE_DISARMED,
                "NODE_TYPE": 4,
                "PROTOCOL_VERSION": 1,
                "CAN_ERROR_STATE": 0,
                "BOOTLOADER_RUNNING": 0,
                "UNSAVED_PARAM_CHANGES": 0,
                "DEBUG_LEVEL": 0,
                "FW_BUILD_ID": 0,
                "UPTIME_S": min(uptime, 65535),
            }, scaling=False)))
            self.next_hb_time = now + HEARTBEAT_PERIOD_S


@dataclass
class EspBenchNodeSim:
    """An ESP32 bench node running firmware/nodes/bench_esp32_arduino: asks for a node ID with its
    MAC, then sends the shared heartbeat and 4x u16 sensor layouts at its own ID."""

    mac: int
    db: cantools.database.can.Database
    hw_target: int = HW_TARGET_ESP32S3
    node_id: int | None = None  # what the board has saved in NVS; None = unconfigured
    boot_time: float = field(default_factory=time.monotonic)
    state: int = STATE_DISARMED
    dropped: bool = False
    next_request_time: float = 0.0
    next_hb_time: float = 0.0
    next_sensor_time: float = 0.0
    requests_sent: int = 0
    pending_echo: can.Message | None = None

    def __post_init__(self) -> None:
        self._rng = random.Random(self.mac)  # same jitter per board, like the MAC-seeded firmware
        self.next_request_time = self.boot_time + 0.1 + self._rng.random() * 0.5

    # -- test controls ------------------------------------------------------

    def drop(self) -> None:
        self.dropped = True

    def revive(self) -> None:
        self.dropped = False

    def reboot(self) -> None:
        self.boot_time = time.monotonic()
        self.state = STATE_DISARMED
        self.next_request_time = self.boot_time + 0.1 + self._rng.random() * 0.5

    def forget(self) -> None:
        """The serial console's `forget`: erase the saved ID and restart."""
        self.node_id = None
        self.reboot()

    # -- bus-event handlers -------------------------------------------------

    def on_frame(self, frame: can.Message, now: float) -> None:
        if self.dropped:
            return
        fid = frame.arbitration_id
        if self.node_id is None:
            if fid in NODE_ID_ASSIGN_IDS:
                d = self.db.decode_message(fid, frame.data, decode_choices=False, scaling=False)
                if d["MAC"] == self.mac and 4 <= d["ASSIGNED_ID"] <= 30:
                    self.node_id = d["ASSIGNED_ID"]
                    self.reboot()  # the firmware saves the ID and restarts
            return
        node, sub = (fid >> 3) & 0x1F, fid & 0x7
        if fid >> 8 == 7 and 2 <= sub <= 4 and node in (0, self.node_id):
            # Host text (class 7, sub 2/3/4 from Orin/RPi/tool): echo it back at sub 5, as the sketch does.
            d = self.db.get_message_by_name("TEXT_TO_NODE_RPI").decode(frame.data, decode_choices=False, scaling=False)
            echo = self.db.get_message_by_name("TEXT_FROM_NODE")
            self.pending_echo = can.Message(arbitration_id=0x700 | (self.node_id << 3) | 5, is_extended_id=False,
                                            data=echo.encode(d, scaling=False))
        elif fid == 0x000:
            self.state = STATE_ESTOP
        elif fid == 0x001 and self.state == STATE_ESTOP:
            self.state = STATE_DISARMED
        elif fid == 0x003:
            d = self.db.decode_message(fid, frame.data, decode_choices=False, scaling=False)
            if d["TARGET_NODE"] in (0, self.node_id):
                if d["COMMAND"] == NMT_RESET:
                    self.reboot()
                elif d["COMMAND"] == NMT_DISARM and self.state == STATE_ARMED:
                    self.state = STATE_DISARMED

    def tick(self, now: float, bus: can.BusABC) -> None:
        if self.dropped:
            return
        if self.pending_echo is not None:
            bus.send(self.pending_echo)
            self.pending_echo = None
        if self.node_id is None:
            if now >= self.next_request_time:
                msg = self.db.get_message_by_name("NODE_ID_REQUEST")
                bus.send(can.Message(arbitration_id=msg.frame_id, is_extended_id=False, data=msg.encode(
                    {"MAC": self.mac, "NODE_TYPE": 3, "HW_TARGET": self.hw_target}, scaling=False)))
                self.requests_sent += 1
                self.next_request_time = now + 0.4 + self._rng.random() * 0.2
            return
        if now >= self.next_hb_time:
            # Same layout as every node heartbeat; the DRIVE1 entry is just the DBC's copy of it.
            msg = self.db.get_message_by_name("DRIVE1_NODE_HB")
            bus.send(can.Message(arbitration_id=0x600 | (self.node_id << 3), is_extended_id=False, data=msg.encode({
                "STATE": self.state,
                "NODE_TYPE": 3,
                "PROTOCOL_VERSION": 1,
                "CAN_ERROR_STATE": 0,
                "BOOTLOADER_RUNNING": 0,
                "UNSAVED_PARAM_CHANGES": 0,
                "DEBUG_LEVEL": 0,
                "FW_BUILD_ID": self.mac & 0xFFFF,
                "UPTIME_S": min(int(now - self.boot_time), 65535),
            }, scaling=False)))
            self.next_hb_time = now + HEARTBEAT_PERIOD_S
        if now >= self.next_sensor_time:
            uptime = int(now - self.boot_time)
            msg = self.db.get_message_by_name("FPGA_SENSORS_A")
            bus.send(can.Message(arbitration_id=0x400 | (self.node_id << 3), is_extended_id=False, data=msg.encode({
                "SENSOR_1": 3500 + (uptime % 200),  # 35-37 degC in 0.01 degC
                "SENSOR_2": 280,                     # free heap, KiB
                "SENSOR_3": 0,
                "SENSOR_4": 0,
            }, scaling=False)))
            self.next_sensor_time = now + ESP_SENSOR_PERIOD_S


class Simulator:
    """Owns the bus, every simulated node, and the receive/dispatch loop."""

    def __init__(self, bus: can.BusABC, db: cantools.database.can.Database | None = None, *,
                 drives: bool = True, fpga: bool = True, esp_macs: list[tuple[int, int]] | None = None):
        """esp_macs: (mac, hw_target) per simulated ESP32 bench node; they start unconfigured."""
        self.bus = bus
        self.db = db or load_db()
        self.drive_nodes = {n: DriveNodeSim(n, self.db) for n in range(1, 7)} if drives else {}
        self.fpga = FpgaNodeSim(self.db) if fpga else None
        self.esp_nodes = [EspBenchNodeSim(mac, self.db, hw) for mac, hw in (esp_macs or [])]
        self._cmd_id_to_node = {node.id_cmd: node for node in self.drive_nodes.values()}

    def inject_drop(self, drive_num: int) -> None:
        self.drive_nodes[drive_num].drop()

    def inject_overcurrent(self, drive_num: int) -> None:
        self.drive_nodes[drive_num].inject_overcurrent()

    def _all_nodes(self):
        yield from self.drive_nodes.values()
        if self.fpga is not None:
            yield self.fpga
        yield from self.esp_nodes

    def dispatch(self, frame: can.Message, now: float) -> None:
        frame_id = frame.arbitration_id
        for esp in self.esp_nodes:
            esp.on_frame(frame, now)
        if frame_id == 0x000:
            for node in self.drive_nodes.values():
                node.on_estop(now)
            return
        if frame_id == 0x001:
            for node in self.drive_nodes.values():
                node.on_estop_release(now)
            return
        if frame_id == 0x002:
            for node in self.drive_nodes.values():
                node.on_sync(now, self.bus)
            return
        if frame_id == 0x003:
            decoded = self.db.decode_message(frame_id, frame.data, decode_choices=False, scaling=False)
            for node in self.drive_nodes.values():
                node.on_nmt(decoded, now)
            return

        node = self._cmd_id_to_node.get(frame_id)
        if node is not None:
            decoded = self.db.decode_message(frame_id, frame.data, decode_choices=False, scaling=False)
            node.on_drive_cmd(decoded, now)

    def step(self, timeout: float = 0.005) -> None:
        """Process at most one incoming frame, then let every node do its periodic work."""
        frame = self.bus.recv(timeout=timeout)
        now = time.monotonic()
        if frame is not None:
            self.dispatch(frame, now)
        for node in self._all_nodes():
            node.tick(now, self.bus)

    def run_for(self, duration_s: float) -> None:
        deadline = time.monotonic() + duration_s
        while time.monotonic() < deadline:
            self.step()

    def run_forever(self) -> None:
        while True:
            self.step()
