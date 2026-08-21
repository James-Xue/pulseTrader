#pragma once
// param_bounds.hpp — shared safety bounds for strategy parameter tuning
//
// Single source of truth for the tunable parameter bounds used by BOTH write
// paths: the AI pipeline (ParamAdvisor, per-cycle delta clamping) and the
// manual control-plane channel (EngineServices::setStrategyParam, hard-limit
// clamping). Header-only so control can use it without a new link edge to
// pulse_ai.
//
// Each bound:
//   1. max_delta — maximum change per AI cycle (±); the LLM delta is clamped
//   2. hard_min  — absolute floor the parameter can reach
//   3. hard_max  — absolute ceiling the parameter can reach

#include <string>
#include <unordered_map>

namespace pulse::ai
{

struct ParamBound
{
    double max_delta{ 0.0 }; ///< Max change per cycle (±).
    double hard_min{ 0.0 };  ///< Absolute minimum value.
    double hard_max{ 0.0 };  ///< Absolute maximum value.
};

/// Default bounds for the 10 AI-tunable parameters. Conservative: limits
/// per-cycle change while allowing the full useful range over many cycles.
[[nodiscard]] inline const std::unordered_map<std::string, ParamBound> &
defaultParamBounds()
{
    static const std::unordered_map<std::string, ParamBound> kBounds = {
        // Order sizing — order_quantity is in base currency (e.g., BTC).
        // A delta of 0.0005 BTC per cycle reaches 0.1 from 0.001 in ~200 cycles.
        { "order_quantity", { 0.0005, 0.0001, 0.1 } },

        // Confidence threshold — minimum signal quality to emit.
        { "min_confidence", { 0.1, 0.1, 0.95 } },

        // Momentum (EMA crossover) — fast EMA window in candle periods.
        { "ema_fast_period", { 2.0, 3.0, 50.0 } },

        // Momentum (EMA crossover) — slow EMA window in candle periods.
        { "ema_slow_period", { 3.0, 10.0, 100.0 } },

        // Mean reversion (Bollinger Bands) — BB window in candle periods.
        { "bb_period", { 3.0, 5.0, 50.0 } },

        // Mean reversion (Bollinger Bands) — standard deviation multiplier.
        { "bb_std_dev", { 0.25, 1.0, 4.0 } },

        // Order book scalping — imbalance threshold (0.0–1.0).
        { "ob_imbalance_threshold", { 0.05, 0.1, 0.9 } },

        // Timing — cooldown between signals per symbol (seconds).
        { "cooldown_seconds", { 5.0, 5.0, 120.0 } },

        // Risk — stop-loss distance as fraction of entry price.
        { "stop_loss_pct", { 0.002, 0.003, 0.05 } },

        // Risk — take-profit target as fraction of entry price.
        { "take_profit_pct", { 0.001, 0.002, 0.03 } },
    };
    return kBounds;
}

} // namespace pulse::ai
