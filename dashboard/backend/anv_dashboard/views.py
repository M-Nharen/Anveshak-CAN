"""Turn the gateway's raw state into what each page shows. Pure functions plus a small rate tracker."""

from __future__ import annotations

from typing import Any

from .settings import Settings

ROLE_MASTER = 2
NODE_STATES = {
    0: "BOOT",
    1: "DISARMED",
    2: "ARMED",
    3: "FAULT",
    4: "ESTOP",
    5: "BOOTLOADER",
}
FAULT_BITS = {
    1: "overcurrent A",
    2: "overcurrent B",
    4: "over-temperature",
    8: "under-voltage",
    16: "encoder A",
    32: "encoder B",
    64: "command timeout",
    128: "E-stop latched",
}
HOSTS = {1: "Orin", 2: "RPi"}


class RateTracker:
    """Per-second rates from successive counter readings."""

    def __init__(self) -> None:
        self._last: dict[str, tuple[float, float]] = {}
        self.rates: dict[str, float] = {}

    def update(self, now: float, counters: dict[str, float]) -> None:
        for key, value in counters.items():
            prev = self._last.get(key)
            if prev is not None and now > prev[0] and value >= prev[1]:
                self.rates[key] = (value - prev[1]) / (now - prev[0])
            self._last[key] = (now, value)


def fault_names(flags: int) -> list[str]:
    return [name for bit, name in FAULT_BITS.items() if flags & bit]


def node_health(n: dict[str, Any]) -> tuple[str, list[str]]:
    """('green'|'yellow'|'red', reasons)."""
    red: list[str] = []
    yellow: list[str] = []
    if not n.get("seen"):
        return "grey", ["never seen"]
    if not n.get("alive"):
        red.append("missing")
    if n.get("state") in (3, 4):
        red.append(NODE_STATES.get(n["state"], "?").lower())
    flags = n.get("fb", {}).get("fault_flags", 0)
    if flags:
        red.append("faults: " + ", ".join(fault_names(flags)))
    if n.get("tec", 0) >= 128 or n.get("rec", 0) >= 128:
        red.append("CAN error passive")
    if n.get("can_error_state", 0) == 2:
        yellow.append("recovered from bus-off")
    if n.get("alive") and n.get("age_ms") is not None and n["age_ms"] > 200:
        yellow.append("heartbeat late")
    if n.get("tec", 0) >= 96 or n.get("rec", 0) >= 96:
        yellow.append("CAN errors rising")
    temps = [v for k, v in n.get("telem", {}).items() if k.startswith("temp_")]
    if temps and max(temps) >= 80:
        red.append("hot")
    elif temps and max(temps) >= 65:
        yellow.append("warm")
    if n.get("state") == 0:
        yellow.append("booting")
    if n.get("reboot_count", 0) > 0:
        yellow.append(f"rebooted {n['reboot_count']}x")
    if red:
        return "red", red + yellow
    if yellow:
        return "yellow", yellow
    return "green", []


def header_view(st: dict[str, Any] | None, control: dict[str, Any]) -> dict[str, Any]:
    if st is None:
        return {"gateway": False, "control": control}
    master = st["believed_master"]
    prompt = None
    if st["leader_lost"]:
        prompt = "Leader lost — choose one"
    elif master == 0:
        prompt = "No leader — choose one"
    return {
        "gateway": True,
        "host_id": st["host_id"],
        "host_name": HOSTS.get(st["host_id"], "?"),
        "role": st["role_name"],
        "is_master": st["role"] == ROLE_MASTER,
        "term": st["term"],
        "leader": master,
        "leader_name": HOSTS.get(master),
        "leader_lost": st["leader_lost"],
        "prompt": prompt,
        "armed": st["armed"],
        "estop": st["estop_latched"],
        "control": control,
    }


def overview_view(
    st: dict[str, Any], rates: RateTracker, cfg: Settings
) -> dict[str, Any]:
    r = rates.rates
    fps = st["bus"]["rx_fps"] + r.get("tx_frames", 0.0)
    load = min(100.0, fps * cfg.avg_frame_bits / cfg.can_bitrate * 100.0)
    return {
        "bus_state": "BUS-OFF" if st["bus"]["bus_off"] else "ok",
        "bus_load_pct": round(load, 1),
        "frames_per_s": round(fps, 1),
        "error_frames_per_s": round(r.get("error_frames", 0.0), 2),
        "tx_failures": st["bus"]["tx_failures"],
        "undecoded_frames": st["bus"]["undecoded_frames"],
        "overruns": st["late_cycles"],  # control cycles that started >2 ms late
        "worst_lateness_us": st["worst_lateness_us"],
        "cycles_sent": st["cycles_sent"],
        "peers": [
            {
                "host": p["host"],
                "name": HOSTS.get(p["host"], "tool"),
                "seen": p["seen"],
                "role": p["role"],
                "term": p["term"],
            }
            for p in st["peers"]
        ],
        "estop": st["estop_latched"],
        "armed": st["armed"],
    }


def nodes_view(st: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for n in st["nodes"]:
        health, reasons = node_health(n)
        telem = n.get("telem", {})
        flags = n.get("fb", {}).get("fault_flags", 0)
        rows.append(
            {
                "node_id": n["node_id"],
                "name": n["name"],
                "state": NODE_STATES.get(n["state"], str(n["state"])),
                "alive": n["alive"],
                "age_ms": None if n["age_ms"] is None else round(n["age_ms"]),
                "fw_build_id": n["fw_build_id"] if n["has_hb"] else None,
                "uptime_s": n["uptime_s"] if n["has_hb"] else None,
                "reboots": n["reboot_count"],
                "tec": n["tec"],
                "rec": n["rec"],
                "temps": {k[5:]: v for k, v in telem.items() if k.startswith("temp_")},
                "supply_v": telem.get("supply_v"),
                "faults": fault_names(flags),
                "health": health,
                "reasons": reasons,
            }
        )
    return rows


def drive_view(st: dict[str, Any], targets: list[float]) -> list[dict[str, Any]]:
    """Per drive: what we last commanded versus what the node reports (node IDs 4-9)."""
    by_id = {n["node_id"]: n for n in st["nodes"]}
    out = []
    for i in range(6):
        node = by_id.get(4 + i, {})
        fb = node.get("fb")
        out.append(
            {
                "drive": i + 1,
                "target": targets[i],
                "measured_a": None if fb is None else fb["speed_a"],
                "measured_b": None if fb is None else fb["speed_b"],
                "state": NODE_STATES.get(node.get("state", -1), "—"),
                "alive": bool(node.get("alive")),
            }
        )
    return out


EVENT_TEXT = {
    "NODE_ALIVE": "{name} joined",
    "NODE_MISSING": "{name} went missing",
    "NODE_REBOOTED": "{name} rebooted",
    "NODE_ID_ASSIGNED": "Board {mac} got node ID {node_id}",
    "LEADER_LOST": "Leader lost",
    "ROLE_CHANGED": "This host is now {role}",
    "ESTOP_SEEN": "E-stop",
    "ESTOP_RELEASED": "E-stop released",
}
ROLE_NAMES = {0: "booting", 1: "standby", 2: "master", 3: "observer"}
EVENT_KIND = {"NODE_ALIVE": "ok", "NODE_ID_ASSIGNED": "ok", "ESTOP_RELEASED": "ok", "NODE_REBOOTED": "warn"}


def event_text(e: dict[str, Any]) -> str:
    fields = {**e, "role": ROLE_NAMES.get(e.get("role", -1), "?"), "mac": e.get("mac", "?")}
    return EVENT_TEXT.get(e["type"], e["type"]).format(**fields)


def bench_view(st: dict[str, Any]) -> dict[str, Any]:
    """The RPi touch screen: every node heard since the gateway started, and what just happened.
    Nodes the rover is built with but that never spoke (drive 1-6, FPGA on a bench) are left out."""
    nodes = []
    for n in st["nodes"]:
        if not n["seen"]:
            continue
        health, reasons = node_health(n)
        nodes.append(
            {
                "node_id": n["node_id"],
                "name": n["name"],
                "hw": n.get("hw_name") or "",
                "mac": n.get("mac"),
                "state": NODE_STATES.get(n["state"], str(n["state"])),
                "alive": n["alive"],
                "age_ms": None if n["age_ms"] is None else round(n["age_ms"]),
                "uptime_s": n["uptime_s"] if n["has_hb"] else None,
                "fw_build_id": n["fw_build_id"] if n["has_hb"] else None,
                "reboots": n["reboot_count"],
                "tec": n["tec"],
                "rec": n["rec"],
                "sensors": n.get("sensors", [])[:4],
                "rx_count": n.get("rx_count", 0),
                "health": health,
                "reasons": reasons,
            }
        )
    events = [
        {
            "key": f"{e['time_ns']}-{e['type']}-{e['node_id']}",
            "age_s": round(e["age_ms"] / 1000.0, 1),
            "text": event_text(e),
            "kind": EVENT_KIND.get(e["type"], "bad" if e["type"] in ("NODE_MISSING", "LEADER_LOST", "ESTOP_SEEN") else "info"),
        }
        for e in reversed(st.get("events", []))
        if e["type"] != "ROLE_CHANGED" or e.get("role") != 1  # standby at start-up is noise
    ]
    return {
        "nodes": nodes,
        "online": sum(1 for n in nodes if n["alive"]),
        "known": len(nodes),
        "allocator": st.get("id_allocator", False),
        "bus_off": st["bus"]["bus_off"],
        "rx_fps": round(st["bus"]["rx_fps"]),
        "error_frames": st["bus"]["error_frames"],
        "events": events[:12],
        "messages": messages_view(st),
    }


def messages_view(st: dict[str, Any]) -> list[dict[str, Any]]:
    """Newest first. Each text this host sent lists the nodes that echoed it back (proof they got it);
    echoes are folded into their message, and only texts a node sent on its own appear separately."""
    texts = st.get("texts", [])
    echoes: dict[int, list[str]] = {}
    for t in texts:
        if t["dir"] == "rx" and t["seq"]:
            echoes.setdefault(t["seq"], []).append(t["name"])
    out = []
    for t in reversed(texts):
        if t["dir"] == "rx" and t["seq"]:
            continue
        msg = {
            "key": f"{t['time_ns']}-{t['dir']}-{t['node_id']}",
            "dir": t["dir"],
            "node_id": t["node_id"],
            "name": t["name"],
            "text": t["text"],
            "age_s": round(t["age_ms"] / 1000.0, 1),
        }
        if t["dir"] == "tx":
            msg["received_by"] = sorted(set(echoes.get(t["seq"], [])))
        out.append(msg)
    return out[:20]
