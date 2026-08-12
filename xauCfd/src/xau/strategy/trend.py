"""EMA-based trend detection over synthetic bars.

We use two exponential moving averages (fast and slow) of bar CLOSE prices:
  * `ema_short` (default 30) — covers ~5 minutes on a 10s bar
  * `ema_long` (default 100)  — covers ~16 minutes on a 10s bar

The trend direction is the SIGN of (ema_short - ema_long) / ema_long:
  * |Δ| < `trend_threshold_pct` (default 0.05%) → RANGE
  * Δ > threshold → UP, confidence = min(1, |Δ| / confidence_full_pct)
  * Δ < -threshold → DOWN, confidence = min(1, |Δ| / confidence_full_pct)

Confidence saturates at `confidence_full_pct` (default 0.30%) — beyond that
we treat the trend as fully established.

EMA uses a recursive formula:
    ema_t = alpha * close_t + (1 - alpha) * ema_{t-1}
with alpha = 2 / (period + 1) (the standard `ta-lib`/pandas definition).
We warm-start by seeding ema_0 with the first close.
"""
from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from enum import Enum
from typing import Iterable


class TrendKind(str, Enum):
    UP = "UP"
    DOWN = "DOWN"
    RANGE = "RANGE"


@dataclass(frozen=True, slots=True)
class Trend:
    """Trend state at the latest bar."""
    kind: TrendKind
    ema_short: Decimal
    ema_long: Decimal
    confidence: float  # 0..1 — how strongly the signal points one way


def _ema(closes: list[Decimal], period: int) -> Decimal | None:
    """Recursive EMA seeded with closes[0]. Returns None if not enough data."""
    if len(closes) < period:
        # With less than `period` bars we still seed and compute a partial EMA,
        # but the result is dominated by the seed and not meaningful. Caller
        # should require at least `period` bars.
        return None
    alpha = Decimal(2) / Decimal(period + 1)
    ema = closes[0]
    for px in closes[1:]:
        ema = alpha * px + (Decimal(1) - alpha) * ema
    return ema


def compute_trend(
    closes: Iterable[Decimal | float | int | str],
    *,
    ema_short: int = 30,
    ema_long: int = 100,
    trend_threshold_pct: float = 0.05,
    confidence_full_pct: float = 0.30,
) -> Trend:
    """Compute trend over a sequence of close prices.

    Returns Trend(kind=RANGE, confidence=0) if we don't have enough bars.
    Otherwise computes ema_short, ema_long and the directional delta.
    """
    closes_d: list[Decimal] = [Decimal(c) for c in closes]
    if len(closes_d) < ema_long or ema_long <= 0 or ema_short <= 0:
        return Trend(kind=TrendKind.RANGE, ema_short=Decimal(0),
                     ema_long=Decimal(0), confidence=0.0)
    e_s = _ema(closes_d, ema_short)
    e_l = _ema(closes_d, ema_long)
    if e_s is None or e_l is None or e_l == 0:
        return Trend(kind=TrendKind.RANGE, ema_short=e_s or Decimal(0),
                     ema_long=e_l or Decimal(0), confidence=0.0)
    delta_pct = float((e_s - e_l) / e_l * 100)
    abs_delta = abs(delta_pct)
    if abs_delta < trend_threshold_pct:
        return Trend(kind=TrendKind.RANGE, ema_short=e_s, ema_long=e_l, confidence=0.0)
    confidence = min(1.0, abs_delta / confidence_full_pct)
    kind = TrendKind.UP if delta_pct > 0 else TrendKind.DOWN
    return Trend(kind=kind, ema_short=e_s, ema_long=e_l, confidence=confidence)