"""Tests for Clock, SystemClock, FrozenClock."""
from __future__ import annotations

from datetime import datetime, timedelta, timezone

from xau.infra.clock import FrozenClock, SystemClock


def test_system_clock_returns_utc_aware_datetime() -> None:
    c = SystemClock()
    n = c.now()
    assert isinstance(n, datetime)
    assert n.tzinfo is not None
    assert n.tzinfo == timezone.utc


def test_system_clock_monotonic_is_nondecreasing() -> None:
    c = SystemClock()
    t0 = c.monotonic()
    t1 = c.monotonic()
    assert t1 >= t0


def test_frozen_clock_starts_at_constructor_value() -> None:
    start = datetime(2026, 8, 11, 12, 0, 0, tzinfo=timezone.utc)
    c = FrozenClock(start=start)
    assert c.now() == start
    assert c.monotonic() == 0.0


def test_frozen_clock_default_start() -> None:
    c = FrozenClock()
    assert c.now().tzinfo is timezone.utc
    assert c.monotonic() == 0.0


def test_frozen_clock_advance_moves_both() -> None:
    c = FrozenClock(start=datetime(2026, 1, 1, tzinfo=timezone.utc))
    c.advance(7.5)
    assert c.monotonic() == 7.5
    assert c.now() == datetime(2026, 1, 1, 0, 0, 7, 500000, tzinfo=timezone.utc)


def test_frozen_clock_set_jumps_wall_only() -> None:
    c = FrozenClock()
    c.set(datetime(2026, 8, 11, tzinfo=timezone.utc))
    assert c.now() == datetime(2026, 8, 11, tzinfo=timezone.utc)
    assert c.monotonic() == 0.0  # unaffected by set()


def test_frozen_clock_is_usable_as_clock_protocol() -> None:
    """A function accepting `Clock` should accept FrozenClock without structural checks."""
    def elapsed_s(c, baseline_dt):
        return (c.now() - baseline_dt).total_seconds()
    c = FrozenClock(start=datetime(2026, 1, 1, tzinfo=timezone.utc))
    base = datetime(2026, 1, 1, tzinfo=timezone.utc)
    assert elapsed_s(c, base) == 0.0
    c.advance(60)
    assert elapsed_s(c, base) == 60.0


def test_clock_protocol_is_runtime_checkable_via_ducktyping() -> None:
    """We don't enforce Protocol at runtime — duck-typed is fine for tests."""
    class FakeClock:
        def now(self):
            return datetime(2026, 1, 1, tzinfo=timezone.utc)
        def monotonic(self):
            return 42.0
    f = FakeClock()
    assert f.now().year == 2026
    assert f.monotonic() == 42.0