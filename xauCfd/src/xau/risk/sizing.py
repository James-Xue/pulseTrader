"""Lot sizing for the volume-profile reversion strategy.

Two functions:

  * `risk_based_lot(equity, risk_pct, sl_usd, leverage)` — compute the maximum
    lot the account can support given a per-trade risk budget. The math:
        position_notional_usd = equity * leverage
        loss_at_sl_per_lot     = sl_usd * OZ_PER_LOT (100 oz per XAU lot)
        risk_budget_usd        = equity * risk_pct
        lots                   = floor(risk_budget_usd / loss_at_sl_per_lot / 100) * 100 / 100
    Returns a Decimal rounded DOWN to 2 decimal places (broker minimum is 0.01).

  * `confidence_weighted(cap_lot, confidence)` — scale the cap by confidence:
        lot = cap_lot * (0.5 + 0.5 * confidence)
    Range: 0.5×cap (confidence=0) to 1.0×cap (confidence=1). This matches the
    user-decided sizing rule (Plan: "Lot sizing: 0.5× to 1.0× of risk-based
    cap, weighted by confidence").

Edge cases:
  * equity <= 0 → returns Decimal(0)
  * sl_usd <= 0 → returns Decimal(0) (no meaningful SL → no position)
  * confidence clamped to [0, 1]
"""
from __future__ import annotations

from decimal import Decimal, ROUND_FLOOR

OZ_PER_LOT = 100  # one XAU CFD lot = 100 oz


def risk_based_lot(
    *,
    equity: Decimal,
    risk_pct: float,
    sl_usd: Decimal,
    leverage: int,
) -> Decimal:
    """Max lot the account can support given a risk budget.

    Args:
      equity: account equity in USD (e.g. Decimal("76.22"))
      risk_pct: fraction of equity to risk on one trade (e.g. 0.0025 = 0.25%)
      sl_usd: stop-loss distance in USD per oz (e.g. Decimal("0.60") = $0.60/oz)
      leverage: per-order leverage (e.g. 500)

    Returns:
      Decimal rounded DOWN to 2 decimals. 0 if no valid position.
    """
    if equity <= 0 or sl_usd <= 0 or leverage <= 0:
        return Decimal("0")
    if not (0 < risk_pct < 1):
        raise ValueError(f"risk_pct must be in (0, 1), got {risk_pct}")
    risk_budget = equity * Decimal(str(risk_pct))
    loss_per_lot = sl_usd * Decimal(OZ_PER_LOT)
    raw_lots = risk_budget / loss_per_lot
    # Round DOWN to 0.01 increments.
    return raw_lots.quantize(Decimal("0.01"), rounding=ROUND_FLOOR)


def confidence_weighted(cap_lot: Decimal, confidence: float) -> Decimal:
    """Scale `cap_lot` by confidence in [0, 1] → range [0.5×cap, 1.0×cap]."""
    if cap_lot <= 0:
        return Decimal("0")
    if confidence < 0:
        confidence = 0.0
    elif confidence > 1:
        confidence = 1.0
    mult = Decimal("0.5") + Decimal("0.5") * Decimal(str(confidence))
    return (cap_lot * mult).quantize(Decimal("0.01"), rounding=ROUND_FLOOR)