#pragma once
// fill_math.hpp — Shared fill arithmetic for the backtest account and the
// paper-trading venue.
//
// Why a shared header (2026-11-25): the offline backtest account
// (BacktestAccount) and the live paper venue must never drift numerically.
// Both delegate here, and tests/unit/core/test_fill_math.cpp pins the golden
// values captured from BacktestAccount's pre-refactor formulas, so "the paper
// venue reuses the backtest fill model" is a checked property rather than a
// claim.
//
// All functions are pure: no clocks, no I/O, no allocation, no globals.
// They live in pulse::core so both pulse_backtest and pulse_execution can use
// them without a new link edge between those libraries.

#include "core/types.hpp"

namespace pulse::core::fill_math
{

/// Default taker rate when the caller passes rate == 0.
///
/// Gate.io session-verified rates (2026-11-14): futures taker 0.05%, spot
/// 0.1%. CFD is NOT a percentage — it is a flat 6 USDT per 100 oz lot charged
/// on the buy leg only, so callers model that separately.
inline constexpr double kDefaultFuturesTakerRate = 0.0005;
inline constexpr double kDefaultSpotTakerRate = 0.001;

/// Fee for one fill.
///
/// Rate semantics are tri-state and preserved verbatim from BacktestAccount:
///   1. rate < 0  → fees disabled (return 0)
///   2. rate == 0 → market default (futures 0.0005, spot 0.001)
///   3. rate > 0  → explicit rate
///
/// The fee base is the notional: quantity × price × quanto_multiplier.
///
/// Parameters:
///   1. quantity          — filled quantity in contracts / base units
///   2. price             — fill price
///   3. quanto_multiplier — contract multiplier (1.0 for spot)
///   4. market            — selects the default rate when rate == 0
///   5. rate              — explicit fee rate, or 0 for the market default
[[nodiscard]] inline double feeFor(double quantity, double price,
                                   double quanto_multiplier, MarketType market,
                                   double rate) noexcept
{
    if (rate < 0.0)
    {
        return 0.0;
    }
    if (0.0 == rate)
    {
        rate = (MarketType::Futures == market) ? kDefaultFuturesTakerRate
                                               : kDefaultSpotTakerRate;
    }
    return quantity * price * quanto_multiplier * rate;
}

/// Gross PnL of one position at `mark`, before fees.
///
/// Long:  (mark - entry) × quantity × quanto
/// Short: (entry - mark) × quantity × quanto
[[nodiscard]] inline double positionPnl(Side side, double entry_price,
                                        double mark, double quantity,
                                        double quanto_multiplier) noexcept
{
    const double diff = (Side::Buy == side) ? (mark - entry_price)
                                            : (entry_price - mark);
    return diff * quantity * quanto_multiplier;
}

/// Initial margin locked by a position.
///
/// A non-positive leverage is treated as "no leverage information" and
/// returns 0 rather than dividing by zero; the caller decides whether that is
/// acceptable (a real order path rejects leverage < 1 before this point).
[[nodiscard]] inline double requiredMargin(double notional,
                                           double leverage) noexcept
{
    return (leverage <= 0.0) ? 0.0 : notional / leverage;
}

} // namespace pulse::core::fill_math
