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

#include <nlohmann/json.hpp>

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

// ---------------------------------------------------------------------------
// Existing trigger detection — is an open price_orders object the floor?
// ---------------------------------------------------------------------------
// The raw /futures/usdt/price_orders listing carries EVERY open trigger on
// the contract, not just this position's stop: take-profits, triggers left
// behind by an earlier position on the same contract, and orders the engine
// never placed. Treating any of them as the protection floor would mark a
// naked position as covered, so the floor test has to look at what the
// trigger actually does.

/// True when `trigger` is an open Gate price_orders object that protects the
/// position identified by `position_side` and its `entry_price`.
///
/// Checks, in order:
///   1. `order_type` is the WHOLE-position close for that side
///      (close-long-position for a long, close-short-position for a short).
///      A trigger that opens exposure or closes the other side cannot be the
///      floor.
///   2. `trigger.rule` selects the loss-side crossing: 2 (price falls to or
///      below the trigger) for a long, 1 (price rises to or above it) for a
///      short. The other rule turns the same order_type into a take-profit.
///   3. `trigger.price` sits strictly on the loss side of `entry_price` AND
///      no farther from it than the configured catastrophe distance
///      (`pct`, plus half a tick of rounding slack). A stop priced off an
///      earlier entry — a 59,400 trigger while the current long entered at
///      70,000 — is therefore rejected: it would let the position lose far
///      more than the floor this engine has to guarantee.
///
/// Parameters:
///   trigger       — one element of the price_orders array
///   position_side — Buy for a long, Sell for a short
///   entry_price   — the position's entry price the stop must protect
///   pct           — configured catastrophe distance (0.01 = 1%); out of
///                   (0, 1) disables the distance bound
///   tick          — contract order_price_round; <= 0 disables the rounding
///                   slack and the distance bound (the level cannot be
///                   verified, so any loss-side stop is better than none)
[[nodiscard]] inline bool isProtectiveStopTrigger(
    const nlohmann::json &trigger, Side position_side, Price entry_price,
    double pct, double tick)
{
    // 1. Whole-position close in the position's own direction.
    if (!trigger.is_object())
    {
        return false;
    }
    const bool is_long = (Side::Buy == position_side);
    const std::string expected_order_type =
        is_long ? "close-long-position" : "close-short-position";
    const auto order_type_it = trigger.find("order_type");
    if (trigger.end() == order_type_it || !order_type_it->is_string())
    {
        return false;
    }
    if (expected_order_type != order_type_it->get<std::string>())
    {
        return false;
    }

    // 2. Loss-side trigger direction.
    const auto trigger_it = trigger.find("trigger");
    if (trigger.end() == trigger_it || !trigger_it->is_object())
    {
        return false;
    }
    const auto rule_it = trigger_it->find("rule");
    if (trigger_it->end() == rule_it || !rule_it->is_number())
    {
        return false;
    }
    const int rule = rule_it->get<int>();
    const int expected_rule = is_long ? 2 : 1;
    if (expected_rule != rule)
    {
        return false;
    }

    // 3. Strictly on the loss side of the entry price. Gate serializes the
    //    price as a string; accept a JSON number as well so a locally built
    //    object is classified the same way.
    const auto price_it = trigger_it->find("price");
    if (trigger_it->end() == price_it)
    {
        return false;
    }
    double price = 0.0;
    if (price_it->is_string())
    {
        price = safeParseDouble(price_it->get<std::string>()).value_or(0.0);
    }
    else if (price_it->is_number())
    {
        price = price_it->get<double>();
    }
    else
    {
        return false;
    }
    if (price <= 0.0)
    {
        return false;
    }
    // 3a. Strictly on the loss side of THIS entry.
    if (is_long ? (price >= entry_price) : (price <= entry_price))
    {
        return false;
    }

    // 3b. Distance bound: the adopted trigger must be at least as protective
    //     as the level this engine would place itself. The plan rounds to the
    //     nearest tick, which can land half a tick beyond the raw level, so
    //     allow that much slack or the engine would reject its own trigger on
    //     the next listing and place a duplicate every revalidation pass.
    const bool level_verifiable =
        entry_price > 0.0 && tick > 0.0 && pct > 0.0 && pct < 1.0;
    if (!level_verifiable)
    {
        return true; // Any loss-side stop beats no stop when levels are unknown.
    }
    const double raw = is_long ? entry_price * (1.0 - pct)
                               : entry_price * (1.0 + pct);
    const double slack = tick * 0.5;
    return is_long ? (price >= raw - slack) : (price <= raw + slack);
}

} // namespace pulse::execution