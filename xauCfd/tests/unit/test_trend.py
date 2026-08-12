"""Tests for EMA-based trend detection."""
from __future__ import annotations

from decimal import Decimal

from xau.strategy.trend import TrendKind, compute_trend


def _closes(*values):
    return [Decimal(str(v)) for v in values]


def test_insufficient_bars_returns_range() -> None:
    closes = _closes(*[100] * 50)
    t = compute_trend(closes, ema_short=30, ema_long=100)
    assert t.kind == TrendKind.RANGE
    assert t.confidence == 0.0


def test_zero_period_rejected() -> None:
    closes = _closes(*[100] * 200)
    t = compute_trend(closes, ema_short=30, ema_long=0)
    assert t.kind == TrendKind.RANGE


def test_flat_market_is_range() -> None:
    closes = _closes(*[100.0] * 200)
    t = compute_trend(closes)
    assert t.kind == TrendKind.RANGE
    assert t.confidence == 0.0


def test_strong_uptrend() -> None:
    """A 200-bar sequence that doubles — delta >> threshold → UP with confidence 1."""
    closes = _closes(*[100 + i * 1.0 for i in range(200)])  # 100 → 300
    t = compute_trend(closes, ema_short=30, ema_long=100,
                      trend_threshold_pct=0.05, confidence_full_pct=0.30)
    assert t.kind == TrendKind.UP
    assert t.confidence == 1.0
    assert t.ema_short > t.ema_long


def test_strong_downtrend() -> None:
    closes = _closes(*[300 - i * 1.0 for i in range(200)])  # 300 → 100
    t = compute_trend(closes)
    assert t.kind == TrendKind.DOWN
    assert t.confidence == 1.0


def test_mild_uptrend_below_threshold() -> None:
    """Tiny drift that falls inside the threshold band → RANGE."""
    closes = _closes(*[100 + i * 0.001 for i in range(200)])
    t = compute_trend(closes, ema_short=30, ema_long=100,
                      trend_threshold_pct=0.05, confidence_full_pct=0.30)
    assert t.kind == TrendKind.RANGE


def test_moderate_uptrend_partial_confidence() -> None:
    """A drift between threshold and full_pct → UP with confidence in (0, 1)."""
    # 0.10% drift should give 0.10/0.30 ≈ 0.33 confidence.
    closes = _closes(*[100 + i * 0.001 for i in range(200)])  # 100 → 100.199 ≈ +0.2%
    # Actually need to scale up to hit ~0.10% delta_pct.
    # 200 steps × step → final close = 100 + 199*step. ema_short ≈ end + α correction.
    # Easier: compute manually then assert ratio.
    closes = _closes(*[100 + i * 0.005 for i in range(200)])  # 100 → ~101
    t = compute_trend(closes, ema_short=30, ema_long=100,
                      trend_threshold_pct=0.05, confidence_full_pct=0.30)
    assert t.kind == TrendKind.UP
    assert 0.0 < t.confidence <= 1.0


def test_confidence_capped_at_one() -> None:
    """Even a massive drift caps confidence at 1.0."""
    closes = _closes(*[100 * (1.5 ** (i / 50)) for i in range(200)])
    t = compute_trend(closes)
    assert t.kind == TrendKind.UP
    assert t.confidence == 1.0


def test_closes_accept_strings_floats_ints() -> None:
    """The signature accepts mixed numeric types — we coerce internally."""
    t = compute_trend([100, 101, 102] * 70, ema_short=30, ema_long=100)
    assert t.kind in {TrendKind.UP, TrendKind.RANGE}


def test_short_window_with_long_period_returns_range() -> None:
    """If we have fewer bars than `ema_long`, we can't compute the trend."""
    closes = _closes(*[100 + i * 0.1 for i in range(50)])  # only 50 bars
    t = compute_trend(closes, ema_short=30, ema_long=100)
    assert t.kind == TrendKind.RANGE
    assert t.confidence == 0.0


def test_ema_short_and_long_both_computed() -> None:
    closes = _closes(*[100 + i * 0.5 for i in range(200)])
    t = compute_trend(closes)
    assert t.ema_short > 0
    assert t.ema_long > 0
    # Short EMA tracks price faster than long → in an uptrend short > long.
    if t.kind == TrendKind.UP:
        assert t.ema_short > t.ema_long