"""S/R reversion planner.

Given a set of `Level` objects and a `Trend`, emit a list of `PlanItem`
pending-order intents. The strategy:

  * For each SUPPORT level (below mid):
      - If trend == DOWN → skip (we only buy on UP/RANGE).
      - Else, place a BUY_LIMIT at (level.price - offset), where the
        offset is uniformly sampled in `[offset_min_usd, offset_max_usd]`
        (in DOLLARS) to avoid all orders clustering at the same price.
  * For each RESISTANCE level (above mid):
      - If trend == UP → skip (we only sell on DOWN/RANGE).
      - Else, place a SELL_LIMIT at (level.price + offset).

The lot size for each plan item is computed by `risk.sizing` from the
confidence score for that level.

Confidence blend:
    conf = 0.4 * distance_score   # higher when level is in the 0.30-0.80 sweet spot
        + 0.3 * volume_score      # higher when this level has more time-at-price
        + 0.3 * trend_alignment   # 1.0 if side aligns with trend, 0.5 otherwise

The planner is pure — it does not place orders or touch the network.
The daemon is responsible for diffing the plan vs the broker and applying
the diff.

User rule (2026-08-11): offset bounds and distance filter are in DOLLARS,
not points. We accept `_usd` values here and convert to points (×100) at
the planner boundary so the rest of the math stays in points.
"""
from __future__ import annotations

import random
from dataclasses import dataclass
from decimal import Decimal
from enum import Enum

from xau.market_data.levels import Level, LevelKind
from xau.market_data.volume_profile import Node
from xau.risk.sizing import confidence_weighted, risk_based_lot
from xau.strategy.trend import Trend, TrendKind


class Side(str, Enum):
    BUY = "BUY"
    SELL = "SELL"


@dataclass(frozen=True, slots=True)
class PlanItem:
    """One pending-order intent."""
    symbol: str
    side: Side
    price: Decimal       # the limit price we'd place at
    lot: Decimal         # computed from confidence × risk cap
    level_price: Decimal # the S/R level this intent is anchored to
    confidence: float    # 0..1 — confidence in this specific level
    reason: str          # human-readable rationale for the log


def _volume_score(level: Level, max_seconds: float) -> float:
    if max_seconds <= 0:
        return 0.5
    return min(1.0, level.seconds / max_seconds)


def _distance_score(level: Level, *, min_usd: float, max_usd: float) -> float:
    """Score peaks 0..1: 1.0 in the middle of the distance band, falling off either side.

    Bounds in DOLLARS; converted to points (×100) internally.
    """
    a_pts = abs(level.distance_pts)
    min_pts = min_usd * 100
    max_pts = max_usd * 100
    if a_pts < min_pts:
        return 0.0
    if a_pts > max_pts:
        return 0.0
    # 1.0 at (min+max)/2, falling off linearly to 0 at the edges.
    mid = (min_pts + max_pts) / 2.0
    span = (max_pts - min_pts) / 2.0
    if span == 0:
        return 1.0
    return max(0.0, 1.0 - abs(a_pts - mid) / span)


def _trend_alignment(side: Side, trend: Trend) -> float:
    """1.0 if the side aligns with the trend (or trend is RANGE); 0.5 otherwise."""
    if trend.kind == TrendKind.RANGE:
        return 1.0
    if side == Side.BUY and trend.kind == TrendKind.UP:
        return 1.0
    if side == Side.SELL and trend.kind == TrendKind.DOWN:
        return 1.0
    return 0.5


def _offset_range_for_trend(
    trend: Trend, *, base_min_usd: float, base_max_usd: float
) -> tuple[float, float]:
    """Trend-conditional offset: tighter when trend confirms, wider in range.

    User rule (2026-08-11): "挂单在 10s 承压线外部 2-5 的位置". Bounds in DOLLARS.
    Trend-following (UP/DOWN aligned with order side): tighter ($2-$3) — let
    momentum carry the order into fill.
    RANGE: wider ($3-$5) — give the reversion room to develop.
    The order side matters: if we are BUYING in a DOWN trend, we are fading
    the trend, so use the wider range. The trend-conditional offset is
    keyed on trend.kind, not on side alignment, since the daemon re-plans
    each cycle and the diff will cancel bad orders quickly.
    """
    if trend.kind == TrendKind.RANGE:
        # wider — mean reversion needs breathing room
        return (max(base_min_usd, 3.0), max(base_max_usd, 5.0))
    # trending: tighter — momentum will pull the order toward fill
    return (min(base_min_usd, 2.0), min(base_max_usd, 3.0))


def plan(
    levels: list[Level],
    trend: Trend,
    *,
    symbol: str,
    equity: Decimal,
    risk_pct: float,
    sl_usd: Decimal,
    max_lot: Decimal,
    min_lot: Decimal,
    leverage: int,
    max_seconds: float | None = None,
    offset_min_usd: float = 2.0,
    offset_max_usd: float = 5.0,
    min_distance_usd: float = 15.0,
    max_distance_usd: float = 80.0,
    rng: random.Random | None = None,
) -> list[PlanItem]:
    """Generate a list of pending-order intents from S/R levels + trend.

    Items with lot < `min_lot` are dropped (server enforces 0.01 lot minimum).

    `offset_*_usd` and `min/max_distance_usd` are in DOLLARS (user rule).
    We convert the offset to integer points (round to 1 pt = $0.01) before
    placing.

    `max_seconds` is the reference for the volume_score normalization —
    typically the top node's seconds from the source profile, so that
    "strongest volume in the window" maps to 1.0. If None, we fall back
    to the max across the supplied levels (per-plan reference).

    `rng` is injectable for deterministic tests; defaults to the stdlib RNG.
    """
    if equity <= 0:
        return []
    rng = rng or random.Random()

    cap_lot = risk_based_lot(
        equity=equity, risk_pct=risk_pct, sl_usd=sl_usd, leverage=leverage,
    )
    if cap_lot < min_lot:
        # Account too small to risk even one minimum lot.
        return []

    if max_seconds is None:
        max_seconds = max((lv.seconds for lv in levels), default=0.0)
    out: list[PlanItem] = []
    one_pt = Decimal("0.01")

    o_min_usd, o_max_usd = _offset_range_for_trend(
        trend, base_min_usd=offset_min_usd, base_max_usd=offset_max_usd
    )

    for lv in levels:
        if lv.kind == LevelKind.SUPPORT and trend.kind == TrendKind.DOWN:
            continue  # don't buy against a strong downtrend
        if lv.kind == LevelKind.RESISTANCE and trend.kind == TrendKind.UP:
            continue  # don't sell against a strong uptrend

        side = Side.BUY if lv.kind == LevelKind.SUPPORT else Side.SELL
        confidence = (
            0.4 * _distance_score(lv, min_usd=min_distance_usd, max_usd=max_distance_usd)
            + 0.3 * _volume_score(lv, max_seconds)
            + 0.3 * _trend_alignment(side, trend)
        )
        confidence = max(0.0, min(1.0, confidence))
        lot = confidence_weighted(cap_lot, confidence)
        if lot < min_lot:
            continue

        # dollars → points (1 pt = $0.01), uniformly sampled in [min, max]
        o_min_pts = max(1, int(round(o_min_usd * 100)))
        o_max_pts = max(o_min_pts, int(round(o_max_usd * 100)))
        offset_pts = rng.randint(o_min_pts, o_max_pts)
        offset = one_pt * offset_pts
        if side == Side.BUY:
            price = lv.price - offset
            reason = (
                f"BUY_LIMIT {offset_pts}pts (${o_min_usd:g}-${o_max_usd:g}) below "
                f"support {lv.price} (conf={confidence:.2f}, seconds={lv.seconds:.1f})"
            )
        else:
            price = lv.price + offset
            reason = (
                f"SELL_LIMIT {offset_pts}pts (${o_min_usd:g}-${o_max_usd:g}) above "
                f"resistance {lv.price} (conf={confidence:.2f}, seconds={lv.seconds:.1f})"
            )
        out.append(PlanItem(
            symbol=symbol,
            side=side,
            price=price,
            lot=lot.quantize(Decimal("0.01")),
            level_price=lv.price,
            confidence=confidence,
            reason=reason,
        ))
    return out