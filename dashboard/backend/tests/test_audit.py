from anv_dashboard.audit import AuditLog


class Clock:
    def __init__(self) -> None:
        self.t = 0.0

    def __call__(self) -> float:
        return self.t


def test_record_and_recent_newest_first():
    log = AuditLog(clock=Clock())
    log.record("rate", "c1", target="cycle", old=50.0, new=40.0)
    log.record("param", "c1", target="pid_kp_a", node=4, old=10.0, new=12.0)
    recent = log.recent(10)
    assert [e["target"] for e in recent] == ["pid_kp_a", "cycle"]


def test_undo_last_pops_the_most_recent_undoable_entry():
    log = AuditLog(clock=Clock())
    log.record("rate", "c1", target="cycle", old=50.0, new=40.0)
    log.record("refused", "c1", target="cycle", attempted=10.0, reason="too low")
    entry = log.undo_last()
    assert entry["target"] == "cycle" and entry["new"] == 40.0
    assert len(log.recent(10)) == 1  # the undone entry is gone, the refusal remains
    assert log.undo_last() is None  # nothing else undoable


def test_recent_caps_the_log_length():
    log = AuditLog(clock=Clock())
    for i in range(250):
        log.record("rate", "c1", target="cycle", old=i, new=i + 1)
    assert len(log.recent(1000)) <= 200
