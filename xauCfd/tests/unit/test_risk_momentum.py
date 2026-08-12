"""Tests for xau.risk.momentum.MomentumTpPolicy."""
from __future__ import annotations

from decimal import Decimal

from xau.gate.orders import SIDE_BUY, SIDE_SELL
from xau.risk.momentum import MomentumTpPolicy
from xau.state.positions import PositionRecord


def _rec(side: int, entry: str = "100.00", entry_time: float = 1000.0) -> PositionRecord:
    return PositionRecord(
        position_id=1, symbol="XAUUSD", side=side,
        entry_price=Decimal(entry), entry_time=entry_time,
    )


def test_buy_adverse_below_threshold_no_close() -> None:
    policy = MomentumTpPolicy(reversal_pts=Decimal("0.48"), window_s=60)
    rec = _rec(SIDE_BUY, entry="100.00", entry_time=1000.0)
    # bid = 99.55 → adverse = 0.45, below threshold 0.48
    assert not policy.should_take_profit(
        rec, now=1010.0, bid=Decimal("99.55"), ask=Decimal("100.10"),
    )


def test_buy_adverse_at_or_above_threshold_closes() -> None:
    policy = MomentumTpPolicy(reversal_pts=Decimal("0.48"), window_s=60)
    rec = _rec(SIDE_BUY, entry="100.00", entry_time=1000.0)
    # bid = 99.52 → adverse = 0.48, equal to threshold
    assert policy.should_take_profit(
        rec, now=1010.0, bid=Decimal("99.52"), ask=Decimal("100.10"),
    )


def test_buy_outside_window_does_not_close() -> None:
    policy = MomentumTpPolicy(reversal_pts=Decimal("0.48"), window_s=60)
    rec = _rec(SIDE_BUY, entry="100.00", entry_time=1000.0)
    # bid 0.5 below → adverse, but at t=1010 elapsed=10 ≤ 60 → close
    assert policy.should_take_profit(
        rec, now=1010.0, bid=Decimal("99.50"), ask=Decimal("100.10"),
    )
    # same adverse, but at t=1061 elapsed=61 > 60 → no close
    assert not policy.should_take_profit(
        rec, now=1061.0, bid=Decimal("99.50"), ask=Decimal("100.10"),
    )


def test_sell_adverse_above_threshold_closes() -> None:
    policy = MomentumTpPolicy(reversal_pts=Decimal("0.48"), window_s=60)
    rec = _rec(SIDE_SELL, entry="100.00", entry_time=1000.0)
    # ask = 100.48 → adverse = 0.48, equal to threshold
    assert policy.should_take_profit(
        rec, now=1010.0, bid=Decimal("99.90"), ask=Decimal("100.48"),
    )


def test_sell_adverse_below_threshold_no_close() -> None:
    policy = MomentumTpPolicy(reversal_pts=Decimal("0.48"), window_s=60)
    rec = _rec(SIDE_SELL, entry="100.00", entry_time=1000.0)
    # ask = 100.47 → adverse = 0.47, just below threshold
    assert not policy.should_take_profit(
        rec, now=1010.0, bid=Decimal("99.90"), ask=Decimal("100.47"),
    )


def test_sell_outside_window_does_not_close() -> None:
    policy = MomentumTpPolicy(reversal_pts=Decimal("0.48"), window_s=60)
    rec = _rec(SIDE_SELL, entry="100.00", entry_time=1000.0)
    assert not policy.should_take_profit(
        rec, now=1100.0, bid=Decimal("99.90"), ask=Decimal("101.00"),
    )


def test_unknown_side_is_silent() -> None:
    policy = MomentumTpPolicy(reversal_pts=Decimal("0.48"), window_s=60)
    rec = _rec(side=0, entry="100.00")
    assert not policy.should_take_profit(
        rec, now=1010.0, bid=Decimal("50.00"), ask=Decimal("150.00"),
    )


def test_window_boundary_is_inclusive() -> None:
    """Exactly at window_s elapsed, still inside the window."""
    policy = MomentumTpPolicy(reversal_pts=Decimal("0.48"), window_s=60)
    rec = _rec(SIDE_BUY, entry="100.00", entry_time=1000.0)
    # elapsed = 60.0 → not > 60 → still in window
    assert policy.should_take_profit(
        rec, now=1060.0, bid=Decimal("99.50"), ask=Decimal("100.10"),
    )