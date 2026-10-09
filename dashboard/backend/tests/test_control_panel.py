"""Hub-level tests for the Rates and Parameters pages, and the audit/undo log: the exact
scenarios B5 must cover (bus-load guard, floors, parameter writes while armed, undo)."""

import asyncio

from anv_dashboard.hub import Hub
from anv_dashboard.settings import Settings
from fakes import FakeBridge, FakeWS, make_state


class Clock:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


def make_hub(**state_over) -> tuple[Hub, FakeBridge, Clock]:
    clock = Clock()
    bridge = FakeBridge(make_state(**state_over) if state_over else None)
    return Hub(Settings(password="pw"), bridge, clock=clock), bridge, clock


async def settle() -> None:
    for _ in range(5):
        await asyncio.sleep(0)


async def connect(hub: Hub) -> FakeWS:
    ws = FakeWS()
    asyncio.create_task(hub.serve(ws))
    await settle()
    ws.say(t="auth", token=hub.sessions.login("pw"), req="a")
    await settle()
    return ws


def run(coro):
    return asyncio.run(coro)


# --- Rates page: floors and the bus-load guard -----------------------------------------------


def test_floor_is_enforced_through_the_hub():
    async def main():
        hub, bridge, _ = make_hub()
        ws = await connect(hub)
        await hub.poll_once()
        ws.say(t="set_rate", rate_id="cycle", hz=24.0, req=1)
        await settle()
        ack = ws.acks()[-1]
        assert ack["ok"] is False and "25" in ack["msg"]
        assert bridge.calls == []  # refused before ever reaching the gateway

    run(main())


def test_load_guard_refuses_an_over_50_percent_change():
    async def main():
        # The catalog's own worst case is well under 50% (docs/architecture.md: "about 4.9%"
        # per fully-loaded drive node), so the guard is exercised against the gateway's actually
        # *measured* bus load (rx_fps) -- a nearly-saturated bus (just under 50%) that a further,
        # otherwise in-range change (host heartbeat up to its 50 Hz ceiling) tips over.
        hub, bridge, _ = make_hub(bus={
            "rx_frames": 1, "tx_frames": 1, "tx_failures": 0, "error_frames": 0, "undecoded_frames": 0,
            "bus_off": False, "rx_fps": 2230.0,
        })  # fmt: skip
        ws = await connect(hub)
        await hub.poll_once()
        ws.say(t="set_rate", rate_id="host_hb", hz=50.0, req=1)
        await settle()
        ack = ws.acks()[-1]
        assert ack["ok"] is False and "50" in ack["msg"] and "%" in ack["msg"]
        assert ("set_host_rate", (None, 50.0)) not in bridge.calls
        assert hub.audit.recent(1)[0]["kind"] == "refused"

        # But the same change is fine on a quiet bus.
        hub2, bridge2, _ = make_hub()
        ws2 = await connect(hub2)
        await hub2.poll_once()
        ws2.say(t="set_rate", rate_id="host_hb", hz=50.0, req=1)
        await settle()
        assert ws2.acks()[-1]["ok"] is True
        assert ("set_host_rate", (None, 50.0)) in bridge2.calls

    run(main())


def test_an_allowed_rate_change_reaches_the_gateway_and_is_logged():
    async def main():
        hub, bridge, _ = make_hub()
        ws = await connect(hub)
        await hub.poll_once()
        ws.say(t="set_rate", rate_id="host_hb", hz=25.0, req=1)
        await settle()
        assert ws.acks()[-1]["ok"] is True
        assert ("set_host_rate", (None, 25.0)) in bridge.calls
        assert hub.audit.recent(1)[0]["kind"] == "rate"
        assert hub.audit.recent(1)[0]["new"] == 25.0

    run(main())


# --- Parameters page: writes refused while armed for protected parameters ----------------------


def test_protected_parameter_write_is_refused_while_armed():
    async def main():
        hub, bridge, _ = make_hub(armed=True)
        ws = await connect(hub)
        await hub.poll_once()
        # pid_kp_a (index 0) is disarmed_only in defs/params/drive.yaml.
        ws.say(t="set_param", node=4, index=0, value=12.0, req=1)
        await settle()
        ack = ws.acks()[-1]
        assert ack["ok"] is False and "disarmed" in ack["msg"]
        assert ("set_param", (4, 0, bridge.params.get(4, {}).get(0, 0))) not in bridge.calls
        assert hub.audit.recent(1)[0]["kind"] == "refused"

    run(main())


def test_protected_parameter_write_succeeds_while_disarmed():
    async def main():
        hub, bridge, _ = make_hub(armed=False)
        ws = await connect(hub)
        await hub.poll_once()
        ws.say(t="set_param", node=4, index=0, value=12.0, req=1)
        await settle()
        assert ws.acks()[-1]["ok"] is True
        assert bridge.params[4][0] != 0  # the raw f32 bits were actually written

    run(main())


def test_unprotected_parameter_write_is_allowed_while_armed():
    async def main():
        hub, bridge, _ = make_hub(armed=True)
        ws = await connect(hub)
        await hub.poll_once()
        # rate_hb_hz (index 12) is not disarmed_only.
        ws.say(t="set_param", node=4, index=12, value=8.0, req=1)
        await settle()
        assert ws.acks()[-1]["ok"] is True
        assert bridge.params[4][12] == 8

    run(main())


def test_node_refusal_status_also_surfaces_as_a_refusal():
    async def main():
        hub, bridge, _ = make_hub(armed=False)
        bridge.refuse_param_set.add((4, 12))
        ws = await connect(hub)
        await hub.poll_once()
        ws.say(t="set_param", node=4, index=12, value=8.0, req=1)
        await settle()
        ack = ws.acks()[-1]
        assert ack["ok"] is False and "status 3" in ack["msg"]

    run(main())


# --- Undo -----------------------------------------------------------------------------------


def test_undo_restores_the_previous_rate():
    async def main():
        hub, bridge, _ = make_hub()
        ws = await connect(hub)
        await hub.poll_once()
        ws.say(t="set_rate", rate_id="host_hb", hz=30.0, req=1)
        await settle()
        assert ws.acks()[-1]["ok"] is True
        ws.say(t="undo_last_change", req=2)
        await settle()
        assert ws.acks()[-1]["ok"] is True
        assert bridge.calls[-1] == ("set_host_rate", (None, 20.0))  # back to the original 20 Hz

    run(main())


def test_undo_restores_the_previous_parameter_value():
    async def main():
        hub, bridge, _ = make_hub(armed=False)
        ws = await connect(hub)
        await hub.poll_once()
        # Seed the cache as if the dashboard had already read the current (default) value.
        hub.node_params[4] = {12: 10}
        ws.say(t="set_param", node=4, index=12, value=7.0, req=1)
        await settle()
        assert bridge.params[4][12] == 7
        ws.say(t="undo_last_change", req=2)
        await settle()
        assert ws.acks()[-1]["ok"] is True
        assert bridge.params[4][12] == 10  # restored

    run(main())


def test_undo_with_nothing_to_undo_is_refused():
    async def main():
        hub, _, _ = make_hub()
        ws = await connect(hub)
        ws.say(t="undo_last_change", req=1)
        await settle()
        assert ws.acks()[-1]["ok"] is False

    run(main())


# --- Firmware page: build mismatches are flagged within a node type -----------------------------


def test_firmware_mismatch_is_flagged():
    async def main():
        from fakes import make_node

        nodes = [make_node(4, node_type=1, fw_build_id=100), make_node(5, node_type=1, fw_build_id=200)]
        hub, _, _ = make_hub(nodes=nodes)
        ws = await connect(hub)
        ws.say(t="page", page="firmware")
        await settle()
        await hub.poll_once()
        await hub.push_once()
        rows = {r["node_id"]: r for r in ws.sent[-1]["firmware"]}
        assert rows[4]["mismatch"] is True and rows[5]["mismatch"] is True

    run(main())
