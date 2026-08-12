"""Wall-clock and monotonic-clock helpers.

The daemon needs `now()` and `monotonic()` for:
  * timestamping log lines and decisions (wall)
  * cadence math / deadlines (monotonic)

Both are wrapped in a `Clock` protocol so tests can substitute a `FrozenClock`
without monkey-patching `time.time` globally.
"""
from __future__ import annotations

import time
from datetime import datetime, timezone
from typing import Protocol


class Clock(Protocol):
    """Injectable time source. Two implementations: SystemClock (default), FrozenClock (tests)."""

    def now(self) -> datetime:
        """Wall-clock UTC time, timezone-aware."""
        ...

    def monotonic(self) -> float:
        """Monotonic seconds since an arbitrary epoch — for cadence math only."""
        ...


class SystemClock:
    """Real-time clock. Use as a singleton or one-per-thread."""

    def now(self) -> datetime:
        return datetime.now(tz=timezone.utc)

    def monotonic(self) -> float:
        return time.monotonic()


class FrozenClock:
    """Test clock — both `now` and `monotonic` advance together.

    Advance by calling `.advance(seconds)`. `monotonic()` returns a counter
    in seconds since the clock was constructed.
    """

    def __init__(self, start: datetime | None = None) -> None:
        self._now = start or datetime(2026, 1, 1, tzinfo=timezone.utc)
        self._mono = 0.0

    def now(self) -> datetime:
        return self._now

    def monotonic(self) -> float:
        return self._mono

    def advance(self, seconds: float) -> None:
        """Move both clocks forward by `seconds`."""
        from datetime import timedelta
        self._now += timedelta(seconds=seconds)
        self._mono += seconds

    def set(self, when: datetime) -> None:
        """Jump the wall clock to `when` without affecting monotonic."""
        self._now = when