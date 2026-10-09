from anv_dashboard.control import ControlManager
from anv_dashboard.settings import Settings


class Clock:
    def __init__(self) -> None:
        self.t = 100.0

    def __call__(self) -> float:
        return self.t


def make() -> tuple[ControlManager, Clock]:
    clock = Clock()
    return ControlManager(Settings(), clock), clock


def speeds(wheels):
    return [w["a"] for w in wheels]


def test_targets_go_to_zero_200ms_after_the_last_message():
    cm, clock = make()
    assert cm.take("a")
    assert cm.drive("a", 1.0, 0.0)
    clock.t += 0.199
    wheels = cm.tick()
    assert wheels is not None and all(s > 0 for s in speeds(wheels))
    clock.t += 0.003  # 202 ms after the last message
    wheels = cm.tick()
    assert wheels is not None and speeds(wheels) == [0.0] * 6
    assert cm.tick() is None  # zero is sent once, then nothing


def test_a_fresh_message_keeps_the_wheels_running():
    cm, clock = make()
    cm.take("a")
    for _ in range(20):  # 20 Hz for a second
        cm.drive("a", 0.5, 0.0)
        clock.t += 0.05
        wheels = cm.tick()
        assert wheels is not None and all(s > 0 for s in speeds(wheels))


def test_a_second_browser_cannot_take_control_while_the_first_holds_it():
    cm, _ = make()
    assert cm.take("a")
    assert not cm.take("b")
    assert not cm.drive("b", 1.0, 0.0)  # and its drive input is ignored
    assert cm.tick() is None
    assert cm.take("a")  # the holder may take again
    assert not cm.release("b")
    assert cm.release("a")
    assert cm.take("b")


def test_control_is_released_when_the_holder_disconnects():
    cm, _ = make()
    cm.take("a")
    cm.drive("a", 1.0, 0.0)
    assert cm.tick() is not None
    cm.on_disconnect("a")
    assert cm.holder is None
    wheels = cm.tick()
    assert wheels is not None and speeds(wheels) == [0.0] * 6  # stops straight away
    assert cm.tick() is None
    assert cm.take("b")


def test_other_browsers_disconnecting_changes_nothing():
    cm, _ = make()
    cm.take("a")
    cm.on_disconnect("b")
    assert cm.holder == "a"


def test_no_drive_before_the_first_message():
    cm, _ = make()
    cm.take("a")
    assert cm.tick() is None


def test_mixing_cap_and_sides():
    cm, _ = make()
    cm.take("a")
    cm.set_limits("a", cap=4.0, accel=7.0)
    cm.drive("a", 1.0, 0.0)
    wheels = cm.tick()
    assert speeds(wheels) == [4.0] * 6
    assert all(
        w["accel"] == 7.0 and w["mode"] == 2 and w["a"] == w["b"] for w in wheels
    )
    cm.drive("a", 0.0, 1.0)  # spin: left back, right forward
    s = speeds(cm.tick())
    assert s[:3] == [-4.0] * 3 and s[3:] == [4.0] * 3
    cm.drive("a", 1.0, 1.0)  # saturates instead of exceeding the cap
    s = speeds(cm.tick())
    assert s[:3] == [0.0] * 3 and s[3:] == [4.0] * 3
    cm.drive("a", 9.0, -9.0)  # out-of-range input is clamped
    assert max(abs(x) for x in speeds(cm.tick())) <= 4.0


def test_speed_steps_stay_in_range_and_need_the_lock():
    cm, _ = make()
    assert not cm.step_speed("a", 1)
    cm.take("a")
    for _ in range(30):
        cm.step_speed("a", -1)
    assert cm.limits.level == Settings().speed_step
    for _ in range(30):
        cm.step_speed("a", 1)
    assert cm.limits.level == 1.0


def test_limits_are_clamped_and_need_the_lock():
    cm, _ = make()
    assert not cm.set_limits("a", 1.0, 1.0)
    cm.take("a")
    cm.set_limits("a", cap=99.0, accel=999.0)
    assert cm.limits.cap == Settings().max_wheel_speed
    assert cm.limits.accel == Settings().max_accel
    cm.set_limits("a", cap=-3.0, accel=None)
    assert cm.limits.cap == 0.0


def test_halt_stops_the_wheels_but_keeps_the_lock():
    cm, _ = make()
    cm.take("a")
    cm.drive("a", 1.0, 0.0)
    assert cm.tick() is not None
    cm.halt()
    assert speeds(cm.tick()) == [0.0] * 6
    assert cm.holder == "a"
