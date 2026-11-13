#pragma once
// ExchangeStopPlan.hpp — pure decision logic for futures protective stops
//
// Kept separate from the REST placement path so the rounding and breach
// rules can be unit-tested without a network or an exchange account.
//
// Gate requirements (verified on testnet 2026-11-13):
//   1. trigger.price must be an integer multiple of the contract's
//      order_price_round — otherwise 1009 AUTO_INVALID_PARAM_TRIGGER_PRICE.
//   2. A long stop must be BELOW the last price and a short stop ABOVE it —
//      otherwise 1029 AUTO_TRIGGER_PRICE_LESS_LAST.

#include "core/types.hpp"

#include <cmath>
#include <optional>
#include <string>

namespace pulse::execution
{

// ---------------------------------------------------------------------------
// ExchangeStopPlan — what to send to Gate price_orders
// ---------------------------------------------------------------------------
struct ExchangeStopPlan
{
    double trigger_price{ 0.0 }; ///< Rounded to the contract's price unit.
    int rule{ 2 };               ///< 1 = price >= trigger, 2 = price <= trigger.
    std::string order_type;      ///< Gate price_orders order_type.
    bool is_long{ false };
};

/// Build a protective-stop plan for a futures position.
///
/// Returns nullopt when no safe stop can be expressed:
///   1. entry_price <= 0, tick <= 0, or pct outside (0, 1)
///   2. the trigger already sits on the wrong side of last_price — i.e. the
///      market has gapped past the stop. last_price <= 0 disables the check
///      (no fresh quote yet); placement may then be rejected by Gate, which
///      the caller treats as a retryable failure.
///
/// Parameters:
///   position_side — Buy for a long, Sell for a short
///   entry_price   — position entry price
///   pct           — stop distance as a fraction (0.01 = 1%)
///   tick          — contract order_price_round (0.1 for BTC_USDT testnet)
///   last_price    — latest traded price, or 0 when unavailable
[[nodiscard]] inline std::optional<ExchangeStopPlan>
planExchangeStop(Side position_side, Price entry_price, double pct, double tick,
                 Price last_price)
{
    // 1. Reject inputs that cannot produce a valid trigger.
    if (entry_price <= 0.0 || tick <= 0.0 || pct <= 0.0 || pct >= 1.0)
    {
        return std::nullopt;
    }

    // 2. Round the raw stop to the exchange price unit. Rounding to the
    //    nearest unit keeps the stop as close to the configured distance as
    //    the contract allows.
    const bool is_long = (Side::Buy == position_side);
    const double raw = is_long ? entry_price * (1.0 - pct)
                               : entry_price * (1.0 + pct);
    const double trigger = std::round(raw / tick) * tick;

    ExchangeStopPlan plan;
    plan.trigger_price = trigger;
    plan.rule = is_long ? 2 : 1;
    plan.order_type = is_long ? "close-long-position" : "close-short-position";
    plan.is_long = is_long;

    // 3. Breach check: a stop that is already through the market would be
    //    rejected (1029) and is meaningless as protection.
    if (last_price > 0.0)
    {
        const bool breached = is_long ? (trigger >= last_price)
                                      : (trigger <= last_price);
        if (breached)
        {
            return std::nullopt;
        }
    }

    return plan;
}

} // namespace pulse::execution