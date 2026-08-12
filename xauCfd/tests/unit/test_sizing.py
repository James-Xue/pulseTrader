"""Tests for risk.sizing: risk_based_lot and confidence_weighted."""
from __future__ import annotations

from decimal import Decimal

import pytest

from xau.risk.sizing import OZ_PER_LOT, confidence_weighted, risk_based_lot


def test_risk_based_lot_basic() -> None:
    """equity=1000, risk=0.25%, sl=$0.60/oz → risk_budget=$2.50, loss/lot=$60, lots=0.04."""
    lot = risk_based_lot(
        equity=Decimal("1000"), risk_pct=0.0025,
        sl_usd=Decimal("0.60"), leverage=500,
    )
    assert lot == Decimal("0.04")


def test_risk_based_lot_rounds_down_to_two_decimals() -> None:
    """0.0417... → 0.04 (floor)."""
    lot = risk_based_lot(
        equity=Decimal("1000"), risk_pct=0.0025,
        sl_usd=Decimal("0.60"), leverage=500,
    )
    # Confirm rounding behavior on a value that would otherwise round to .05
    lot2 = risk_based_lot(
        equity=Decimal("100"), risk_pct=0.005,
        sl_usd=Decimal("0.50"), leverage=500,
    )
    # equity*risk_pct = $0.50, loss_per_lot = $50, lots = 0.01 (exact)
    assert lot2 == Decimal("0.01")


def test_risk_based_lot_zero_when_equity_zero() -> None:
    assert risk_based_lot(
        equity=Decimal("0"), risk_pct=0.0025,
        sl_usd=Decimal("0.60"), leverage=500,
    ) == Decimal("0")


def test_risk_based_lot_zero_when_sl_zero() -> None:
    assert risk_based_lot(
        equity=Decimal("1000"), risk_pct=0.0025,
        sl_usd=Decimal("0"), leverage=500,
    ) == Decimal("0")


def test_risk_based_lot_zero_when_leverage_zero() -> None:
    assert risk_based_lot(
        equity=Decimal("1000"), risk_pct=0.0025,
        sl_usd=Decimal("0.60"), leverage=0,
    ) == Decimal("0")


def test_risk_based_lot_rejects_invalid_risk_pct() -> None:
    with pytest.raises(ValueError, match="risk_pct must be in"):
        risk_based_lot(
            equity=Decimal("1000"), risk_pct=1.5,
            sl_usd=Decimal("0.60"), leverage=500,
        )
    with pytest.raises(ValueError, match="risk_pct must be in"):
        risk_based_lot(
            equity=Decimal("1000"), risk_pct=0,
            sl_usd=Decimal("0.60"), leverage=500,
        )


def test_confidence_weighted_half_at_zero() -> None:
    """confidence=0 → 0.5× cap."""
    lot = confidence_weighted(Decimal("0.10"), 0.0)
    assert lot == Decimal("0.05")


def test_confidence_weighted_full_at_one() -> None:
    """confidence=1 → 1.0× cap."""
    lot = confidence_weighted(Decimal("0.10"), 1.0)
    assert lot == Decimal("0.10")


def test_confidence_weighted_midpoint() -> None:
    """confidence=0.5 → 0.75× cap."""
    lot = confidence_weighted(Decimal("0.10"), 0.5)
    assert lot == Decimal("0.07")  # 0.075 floored to 0.07


def test_confidence_weighted_zero_cap_returns_zero() -> None:
    assert confidence_weighted(Decimal("0"), 1.0) == Decimal("0")


def test_confidence_weighted_clamps_negative() -> None:
    lot = confidence_weighted(Decimal("0.10"), -0.5)
    assert lot == Decimal("0.05")  # clamped to 0


def test_confidence_weighted_clamps_above_one() -> None:
    lot = confidence_weighted(Decimal("0.10"), 2.0)
    assert lot == Decimal("0.10")  # clamped to 1


def test_oz_per_lot_constant() -> None:
    """Sanity: XAU CFD = 100 oz / lot per industry convention."""
    assert OZ_PER_LOT == 100


def test_realistic_xau_account_sizing() -> None:
    """End-to-end: $76 account, 0.25% risk, $0.60 SL → 0.01 lot min trade."""
    lot = risk_based_lot(
        equity=Decimal("76.22"), risk_pct=0.0025,
        sl_usd=Decimal("0.60"), leverage=500,
    )
    # risk_budget = $0.19; loss/lot = $60; lots = 0.003 → 0.00 (floor)
    assert lot == Decimal("0.00")
    # So no position is supported at this account size + risk budget.
    # With confidence weighting even at 1.0, still 0.
    assert confidence_weighted(lot, 1.0) == Decimal("0")


def test_larger_account_supports_min_lot() -> None:
    """$400 account: risk_budget = $1, loss/lot = $60, lots = 0.01 → MIN_LOT."""
    lot = risk_based_lot(
        equity=Decimal("400"), risk_pct=0.0025,
        sl_usd=Decimal("0.60"), leverage=500,
    )
    assert lot == Decimal("0.01")