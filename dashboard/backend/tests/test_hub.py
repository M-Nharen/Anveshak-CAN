import asyncio

from anv_dashboard.hub import Hub
from anv_dashboard.settings import Settings
from fakes import FakeBridge, FakeWS


class Clock:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


def make_hub() -> tuple[Hub, FakeBridge, Clock]:
    clock = Clock()
    bridge = FakeBridge()
    return Hub(Settings(password="pw"), bridge, clock=clock), bridge, clock


async def settle() -> None:
    for _ in range(5):
        await asyncio.sleep(0)


async def connect(hub: Hub, login: bool = True) -> FakeWS:
    ws = FakeWS()
    asyncio.create_task(hub.serve(ws))
    await settle()
    if login:
        ws.say(t="auth", token=hub.sessions.login("pw"), req="a")
        await settle()
    return ws


def run(coro):
    return asyncio.run(coro)


def test_viewing_needs_no_login_but_commands_do():
    async def main():
        hub, bridge, _ = make_hub()
        ws = await connect(hub, login=False)
        ws.say(t="arm", req=1)
        ws.say(t="take_control", req=2)
        ws.say(t="request_lead", host=1, req=3)
        await settle()
        assert [a["ok"] for a in ws.acks()] == [False, False, False]
        assert all(a["msg"] == "login required" for a in ws.acks())
        assert bridge.calls == []
        await hub.poll_once()
        await hub.push_once()
        assert (
            ws.sent[-1]["t"] == "state" and ws.sent[-1]["authed"] is False
        )  # but it can watch
        assert ws.sent[-1]["overview"]["bus_state"] == "ok"

    run(main())


def test_bad_token_does_not_log_in():
    async def main():
        hub, _, _ = make_hub()
        ws = await connect(hub, login=False)
        ws.say(t="auth", token="forged", req=1)
        await settle()
        assert ws.acks()[0]["ok"] is False

    run(main())


def test_estop_works_without_login_and_stops_driving():
    async def main():
        hub, bridge, _ = make_hub()
        viewer = await connect(hub, login=False)
        viewer.say(t="estop", req=1)
        await settle()
        assert ("estop", None) in bridge.calls and viewer.acks()[0]["ok"] is True

    run(main())


def test_commands_reach_the_gateway_once_logged_in():
    async def main():
        hub, bridge, _ = make_hub()
        ws = await connect(hub)
        ws.say(t="request_lead", host=2, req=1)
        ws.say(t="arm", req=2)
        ws.say(t="disarm", req=3)
        ws.say(t="release_estop", req=4)
        ws.say(t="request_lead", host=9, req=5)
        await settle()
        assert bridge.calls == [
            ("request_lead", 2),
            ("arm", None),
            ("disarm", None),
            ("release_estop", None),
        ]
        acks = {a["req"]: a for a in ws.acks()}
        assert acks[1]["ok"] and acks[2]["ok"] and not acks[5]["ok"]

    run(main())


def test_only_one_browser_holds_control_and_it_goes_with_the_browser():
    async def main():
        hub, _, _ = make_hub()
        a, b = await connect(hub), await connect(hub)
        a.say(t="take_control", req=1)
        await settle()
        b.say(t="take_control", req=1)
        b.say(t="drive", fwd=1.0, turn=0.0)
        await settle()
        assert a.acks()[-1]["ok"] is True
        assert [x["ok"] for x in b.acks()[1:]] == [
            False,
            False,
        ]  # take refused, drive refused
        await hub.push_once()
        assert a.sent[-1]["header"]["control"] == {
            "source": "dashboard",
            "mine": True,
            "taken": True,
        }
        assert b.sent[-1]["header"]["control"] == {
            "source": "dashboard",
            "mine": False,
            "taken": True,
        }

        a.hang_up()  # tab closed
        await settle()
        assert hub.control.holder is None
        b.say(t="take_control", req=2)
        await settle()
        assert b.acks()[-1]["ok"] is True

    run(main())


def test_hold_to_run_through_the_hub():
    async def main():
        hub, bridge, clock = make_hub()
        ws = await connect(hub)
        ws.say(t="take_control", req=1)
        ws.say(t="drive", fwd=1.0, turn=0.0)
        await settle()
        await hub.control_once()
        assert bridge.targets and bridge.targets[-1][0]["a"] > 0
        clock.t += 0.25  # the key was released and nothing has arrived since
        await hub.control_once()
        assert bridge.targets[-1][0]["a"] == 0.0
        n = len(bridge.targets)
        await hub.control_once()
        assert len(bridge.targets) == n  # nothing more to send

        ws.say(t="drive", fwd=1.0, turn=0.0)
        await settle()
        await hub.control_once()
        ws.hang_up()
        await settle()
        await hub.control_once()  # the closed browser's wheels stop at once
        assert bridge.targets[-1][0]["a"] == 0.0

    run(main())


def test_drive_page_snapshot_and_limits():
    async def main():
        hub, _, _ = make_hub()
        ws = await connect(hub)
        ws.say(t="page", page="drive")
        ws.say(t="take_control", req=1)
        ws.say(t="limits", cap=2.5, accel=4.0, req=2)
        ws.say(t="speed_step", dir=-1, req=3)
        await settle()
        await hub.poll_once()
        await hub.push_once()
        d = ws.sent[-1]["drive"]
        assert (
            len(d["wheels"]) == 6
            and d["limits"]["cap"] == 2.5
            and d["limits"]["accel"] == 4.0
        )
        assert abs(d["limits"]["level"] - 0.9) < 1e-9
        assert "overview" not in ws.sent[-1]  # only what the open page needs

    run(main())


def test_push_rate_per_page():
    async def main():
        hub, _, clock = make_hub()
        ws = await connect(hub)
        await hub.poll_once()
        n = len(ws.sent)
        await hub.push_once()
        await hub.push_once()  # same instant: no second push
        assert len(ws.sent) == n + 1
        clock.t += 0.06  # < 100 ms: overview is limited to 10 Hz
        await hub.push_once()
        assert len(ws.sent) == n + 1
        clock.t += 0.05
        await hub.push_once()
        assert len(ws.sent) == n + 2
        ws.say(t="page", page="drive")
        await settle()
        clock.t += 0.05  # drive page: 20 Hz
        await hub.push_once()
        assert len(ws.sent) == n + 3

    run(main())


def test_gateway_down_is_reported_not_fatal():
    async def main():
        hub, bridge, _ = make_hub()
        ws = await connect(hub)
        await hub.poll_once()
        bridge.fail = True
        await hub.poll_once()
        await hub.control_once()
        await hub.push_once()
        assert ws.sent[-1]["header"]["gateway"] is False
        ws.say(t="take_control", req=1)
        ws.say(t="drive", fwd=1.0, turn=0.0)
        await settle()
        await (
            hub.control_once()
        )  # set_targets fails: swallowed, the gateway zeroes stale targets itself

    run(main())


def test_garbage_messages_do_not_kill_the_connection():
    async def main():
        hub, _, _ = make_hub()
        ws = await connect(hub)
        ws.inbox.put_nowait("not json")
        ws.inbox.put_nowait("[1,2]")
        ws.say(t="warp")
        ws.say(t="limits", cap="fast", req=9)
        ws.say(t="take_control", req=10)
        await settle()
        assert ws.acks()[-1]["ok"] is True
        assert hub.clients

    run(main())


def test_set_debug_and_save_flight_recording_need_login():
    async def main():
        hub, bridge, _ = make_hub()
        anon = await connect(hub, login=False)
        anon.say(t="set_debug", level=2, req=1)
        anon.say(t="save_flight_recording", req=2)
        await settle()
        acks = {a["req"]: a for a in anon.acks()}
        assert not acks[1]["ok"] and not acks[2]["ok"]
        assert bridge.calls == []

        ws = await connect(hub)
        ws.say(t="set_debug", node_id=5, level=2, category_mask=7, auto_off_min=10, req=1)
        ws.say(t="save_flight_recording", reason="test", req=2)
        ws.say(t="set_debug", req=3)  # no level: rejected before it reaches the bridge
        await settle()
        acks = {a["req"]: a for a in ws.acks()}
        assert acks[1]["ok"] and acks[2]["ok"]
        assert not acks[3]["ok"]
        assert bridge.calls == [
            ("set_debug", (5, 2, 7, 10)),
            ("save_flight_recording", "test"),
        ]

    run(main())


def test_errors_page_snapshot_picks_up_polled_events():
    async def main():
        hub, bridge, _ = make_hub()
        bridge.events_value = [
            {
                "seq": 1, "time_ns": 1, "type": "NODE_EVENT", "node_id": 4, "term": 0, "role": 0,
                "code": 0x0304, "severity": 1, "detail": 0, "cleared": False,
            },
            {
                "seq": 2, "time_ns": 2, "type": "GATEWAY_LINK_LOST", "node_id": 0, "term": 0, "role": 0,
                "code": 0x8010, "severity": 1, "detail": 0, "cleared": False,
            },
        ]  # fmt: skip
        ws = await connect(hub)
        ws.say(t="page", page="errors")
        await settle()
        await hub.poll_once()
        await hub.push_once()
        page = ws.sent[-1]["errors"]
        names = {e["name"] for e in page["events"]}
        assert names == {"STALL_DETECTED", "GATEWAY_LINK_LOST"}
        assert any(e["name"] == "STALL_DETECTED" for e in page["active"])
        assert page["bus_errors"][0]["type"] == "ack"
        assert hub._events_seq == 2

        # Polling again with no new events must not duplicate what is already cached.
        await hub.poll_once()
        assert len(hub.events) == 2

    run(main())
