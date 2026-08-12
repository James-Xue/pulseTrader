"""Tests for the S/R reversion planner."""
from __future__ import annotations

import random
from decimal import Decimal

from xau.market_data.levels import Level, LevelKind
from xau.strategy.sr_reversion import Side, plan
from xau.strategy.trend import Trend, TrendKind


def _trend(kind, conf=0.5) -> Trend:
    return Trend(kind=kind, ema_short=Decimal("100"), ema_long=Decimal("100"), confidence=conf)


def _level(price: str, kind: LevelKind, seconds: float = 5.0) -> Level:
    diff = Decimal(price) - Decimal("4368.00")
    return Level(price=Decimal(price), kind=kind, seconds=seconds,
                 distance_pts=float(diff) * 100)


def test_no_levels_no_plan() -> None:
    items = plan([], _trend(TrendKind.RANGE),
                 symbol="XAUUSD", equity=Decimal("1000"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(0))
    assert items == []


def test_zero_equity_no_plan() -> None:
    levels = [_level("4367.50", LevelKind.SUPPORT)]
    items = plan(levels, _trend(TrendKind.RANGE),
                 symbol="XAUUSD", equity=Decimal("0"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(0))
    assert items == []


def test_range_trend_plans_both_sides() -> None:
    levels = [
        _level("4367.50", LevelKind.SUPPORT),  # 50 pts below
        _level("4368.50", LevelKind.RESISTANCE),  # 50 pts above
    ]
    items = plan(levels, _trend(TrendKind.RANGE),
                 symbol="XAUUSD", equity=Decimal("10000"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(0))
    sides = {it.side for it in items}
    assert sides == {Side.BUY, Side.SELL}
    assert len(items) == 2


def test_downtrend_skips_support() -> None:
    """DOWN trend → only SELL_LIMITs (no buying into the trend)."""
    levels = [
        _level("4367.50", LevelKind.SUPPORT),
        _level("4368.50", LevelKind.RESISTANCE),
    ]
    items = plan(levels, _trend(TrendKind.DOWN),
                 symbol="XAUUSD", equity=Decimal("10000"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(0))
    assert all(it.side == Side.SELL for it in items)
    assert len(items) == 1


def test_uptrend_skips_resistance() -> None:
    """UP trend → only BUY_LIMITs (no selling into the trend)."""
    levels = [
        _level("4367.50", LevelKind.SUPPORT),
        _level("4368.50", LevelKind.RESISTANCE),
    ]
    items = plan(levels, _trend(TrendKind.UP),
                 symbol="XAUUSD", equity=Decimal("10000"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(0))
    assert all(it.side == Side.BUY for it in items)
    assert len(items) == 1


def test_buy_limit_offset_below_support() -> None:
    """BUY_LIMIT price must be $3-$5 BELOW the support level (RANGE trend)."""
    levels = [_level("4367.50", LevelKind.SUPPORT)]
    items = plan(levels, _trend(TrendKind.RANGE),
                 symbol="XAUUSD", equity=Decimal("10000"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(42))
    assert len(items) == 1
    assert items[0].side == Side.BUY
    # 1 pt = $0.01 → $3-$5 = 300-500 pts
    diff_pts = float(Decimal("4367.50") - items[0].price) * 100
    assert 300.0 <= diff_pts <= 500.0


def test_sell_limit_offset_above_resistance() -> None:
    """SELL_LIMIT price must be $3-$5 ABOVE the resistance level (RANGE trend)."""
    levels = [_level("4368.50", LevelKind.RESISTANCE)]
    items = plan(levels, _trend(TrendKind.RANGE),
                 symbol="XAUUSD", equity=Decimal("10000"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(42))
    assert len(items) == 1
    assert items[0].side == Side.SELL
    diff_pts = float(items[0].price - Decimal("4368.50")) * 100
    assert 300.0 <= diff_pts <= 500.0


def test_lot_scales_with_confidence() -> None:
    """Higher-confidence level gets a larger lot (closer to cap)."""
    strong_level = _level("4367.50", LevelKind.SUPPORT, seconds=10.0)
    weak_level = _level("4367.50", LevelKind.SUPPORT, seconds=0.1)
    # Pass the SAME max_seconds (10.0) for both so the strong level
    # gets volume_score=1.0 and the weak level gets 0.1/10=0.01.
    items_strong = plan([strong_level], _trend(TrendKind.RANGE, conf=0.8),
                        symbol="XAUUSD", equity=Decimal("4000"),
                        risk_pct=0.0025, sl_usd=Decimal("0.60"),
                        max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                        leverage=500, max_seconds=10.0,
                        rng=random.Random(0))
    items_weak = plan([weak_level], _trend(TrendKind.RANGE, conf=0.8),
                      symbol="XAUUSD", equity=Decimal("4000"),
                      risk_pct=0.0025, sl_usd=Decimal("0.60"),
                      max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                      leverage=500, max_seconds=10.0,
                      rng=random.Random(0))
    assert items_strong[0].confidence > items_weak[0].confidence
    assert items_strong[0].lot > items_weak[0].lot


def test_lot_below_min_lot_is_dropped() -> None:
    """If cap × 0.5 < min_lot, the level is skipped."""
    levels = [_level("4367.50", LevelKind.SUPPORT)]
    # equity too small → cap will be tiny.
    items = plan(levels, _trend(TrendKind.RANGE),
                 symbol="XAUUSD", equity=Decimal("50"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(0))
    assert items == []


def test_confidence_in_unit_interval() -> None:
    levels = [_level("4367.50", LevelKind.SUPPORT, seconds=8.0)]
    items = plan(levels, _trend(TrendKind.UP),
                 symbol="XAUUSD", equity=Decimal("4000"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(0))
    assert 0.0 <= items[0].confidence <= 1.0


def test_plan_includes_level_price_for_audit() -> None:
    """The level_price field lets the log show what level the plan references."""
    levels = [_level("4367.50", LevelKind.SUPPORT)]
    items = plan(levels, _trend(TrendKind.RANGE),
                 symbol="XAUUSD", equity=Decimal("10000"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(0))
    assert items[0].level_price == Decimal("4367.50")


def test_reason_string_includes_key_fields() -> None:
    levels = [_level("4367.50", LevelKind.SUPPORT, seconds=7.5)]
    items = plan(levels, _trend(TrendKind.RANGE),
                 symbol="XAUUSD", equity=Decimal("10000"),
                 risk_pct=0.0025, sl_usd=Decimal("0.60"),
                 max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                 leverage=500, rng=random.Random(0))
    assert "BUY_LIMIT" in items[0].reason
    assert "support" in items[0].reason
    assert "7.5" in items[0].reason


def test_deterministic_with_seeded_rng() -> None:
    """Same seed → same offsets."""
    levels = [_level("4367.50", LevelKind.SUPPORT)]
    items1 = plan(levels, _trend(TrendKind.RANGE),
                  symbol="XAUUSD", equity=Decimal("10000"),
                  risk_pct=0.0025, sl_usd=Decimal("0.60"),
                  max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                  leverage=500, rng=random.Random(99))
    items2 = plan(levels, _trend(TrendKind.RANGE),
                  symbol="XAUUSD", equity=Decimal("10000"),
                  risk_pct=0.0025, sl_usd=Decimal("0.60"),
                  max_lot=Decimal("0.10"), min_lot=Decimal("0.01"),
                  leverage=500, rng=random.Random(99))
    assert items1[0].price == items2[0].price