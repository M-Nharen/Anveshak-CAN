from anv_dashboard.auth import Sessions


class Clock:
    def __init__(self) -> None:
        self.t = 0.0

    def __call__(self) -> float:
        return self.t


def test_right_password_gives_a_working_token():
    s = Sessions("secret", 60, Clock())
    token = s.login("secret")
    assert token and s.valid(token)


def test_wrong_password_and_unknown_tokens_are_refused():
    s = Sessions("secret", 60, Clock())
    assert s.login("nope") is None
    assert not s.valid("made-up") and not s.valid(None) and not s.valid("")


def test_tokens_expire_and_can_be_revoked():
    clock = Clock()
    s = Sessions("secret", 60, clock)
    t1, t2 = s.login("secret"), s.login("secret")
    assert t1 != t2
    s.logout(t2)
    assert not s.valid(t2)
    clock.t = 61
    assert not s.valid(t1)


def test_too_many_wrong_guesses_lock_out_for_a_minute():
    clock = Clock()
    s = Sessions("secret", 60, clock)
    for _ in range(10):
        assert s.login("bad") is None
    assert s.login("secret") is None  # even the right one, while locked out
    clock.t = 61
    assert s.login("secret")


def test_a_password_is_generated_when_none_is_configured():
    s = Sessions(None, 60, Clock())
    assert s.generated and len(s.password) >= 8
    assert s.login(s.password)
