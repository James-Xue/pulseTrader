// param_advisor.cpp — Delta validation + atomic StrategyParams writes (Layer 4 AI Analysis)
//
// Implementation of the ParamAdvisor that applies AI-recommended deltas to strategy
// parameters with safety-bound clamping.
//
// Default safety bounds (all 10 AI-tunable parameters):
//   - order_quantity:          max_delta ±0.0005,  hard [0.0001, 0.1]
//   - min_confidence:          max_delta ±0.1,     hard [0.1, 0.95]
//   - ema_fast_period:         max_delta ±2.0,     hard [3, 50]
//   - ema_slow_period:         max_delta ±3.0,     hard [10, 100]
//   - bb_period:               max_delta ±3.0,     hard [5, 50]
//   - bb_std_dev:              max_delta ±0.25,    hard [1, 4]
//   - ob_imbalance_threshold:  max_delta ±0.05,    hard [0.1, 0.9]
//   - cooldown_seconds:        max_delta ±5.0,     hard [5, 120]
//   - stop_loss_pct:           max_delta ±0.002,   hard [0.003, 0.05]
//   - take_profit_pct:         max_delta ±0.001,   hard [0.002, 0.03]
//
// These defaults are conservative — they allow gradual tuning over multiple AI
// cycles while preventing any single cycle from making dangerous changes.

#include "pulse/ai/param_advisor.hpp"

#include "pulse/logging/logger.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// ParamAdvisor constructor — initializes default safety bounds
//
// Each bound is chosen to:
//   1. Allow meaningful per-cycle adjustments (not too small)
//   2. Prevent dangerous extremes (not too large)
//   3. Cover the full useful operating range in hard_min/hard_max
// ---------------------------------------------------------------------------
ParamAdvisor::ParamAdvisor()
{
    // Order sizing — order_quantity is in base currency (e.g., BTC)
    // A delta of 0.0005 BTC per cycle allows reaching 0.1 from 0.001 in ~200 cycles
    bounds_["order_quantity"] = ParamBound{0.0005, 0.0001, 0.1};

    // Confidence threshold — controls the minimum signal quality to emit
    bounds_["min_confidence"] = ParamBound{0.1, 0.1, 0.95};

    // Momentum (EMA crossover) — fast EMA window in candle periods
    bounds_["ema_fast_period"] = ParamBound{2.0, 3.0, 50.0};

    // Momentum (EMA crossover) — slow EMA window in candle periods
    bounds_["ema_slow_period"] = ParamBound{3.0, 10.0, 100.0};

    // Mean reversion (Bollinger Bands) — BB window in candle periods
    bounds_["bb_period"] = ParamBound{3.0, 5.0, 50.0};

    // Mean reversion (Bollinger Bands) — standard deviation multiplier
    bounds_["bb_std_dev"] = ParamBound{0.25, 1.0, 4.0};

    // Order book scalping — imbalance threshold (0.0–1.0)
    bounds_["ob_imbalance_threshold"] = ParamBound{0.05, 0.1, 0.9};

    // Timing — cooldown between signals per symbol (seconds)
    bounds_["cooldown_seconds"] = ParamBound{5.0, 5.0, 120.0};

    // Risk — stop-loss distance as fraction of entry price
    bounds_["stop_loss_pct"] = ParamBound{0.002, 0.003, 0.05};

    // Risk — take-profit target as fraction of entry price
    bounds_["take_profit_pct"] = ParamBound{0.001, 0.002, 0.03};
}

// ---------------------------------------------------------------------------
// apply — iterate through all 10 deltas and apply each with clamping
//
// The 10 parameter deltas are processed individually (not in a generic loop)
// because C++ does not allow easy pointer-to-member access for std::atomic<T>
// fields without undefined behavior. The explicit approach is safer and clearer.
//
// Zero deltas are skipped to avoid unnecessary atomic read-modify-write cycles
// and spurious OnParamUpdate events.
// ---------------------------------------------------------------------------
std::vector<heartbeat::OnParamUpdate> ParamAdvisor::apply(
        const AnalysisResult &result,
        strategy::StrategyParams &params)
{
    std::vector<heartbeat::OnParamUpdate> events;
    events.reserve(10); // at most 10 parameter updates per cycle

    const auto &d = result.param_deltas;

    // 1. order_quantity
    if (0.0 != d.order_quantity_delta)
    {
        events.push_back(apply_one("order_quantity", d.order_quantity_delta,
                                   bounds_.at("order_quantity"), params.order_quantity));
    }

    // 2. min_confidence
    if (0.0 != d.min_confidence_delta)
    {
        events.push_back(apply_one("min_confidence", d.min_confidence_delta,
                                   bounds_.at("min_confidence"), params.min_confidence));
    }

    // 3. ema_fast_period
    if (0.0 != d.ema_fast_period_delta)
    {
        events.push_back(apply_one("ema_fast_period", d.ema_fast_period_delta,
                                   bounds_.at("ema_fast_period"), params.ema_fast_period));
    }

    // 4. ema_slow_period
    if (0.0 != d.ema_slow_period_delta)
    {
        events.push_back(apply_one("ema_slow_period", d.ema_slow_period_delta,
                                   bounds_.at("ema_slow_period"), params.ema_slow_period));
    }

    // 5. bb_period
    if (0.0 != d.bb_period_delta)
    {
        events.push_back(apply_one("bb_period", d.bb_period_delta,
                                   bounds_.at("bb_period"), params.bb_period));
    }

    // 6. bb_std_dev
    if (0.0 != d.bb_std_dev_delta)
    {
        events.push_back(apply_one("bb_std_dev", d.bb_std_dev_delta,
                                   bounds_.at("bb_std_dev"), params.bb_std_dev));
    }

    // 7. ob_imbalance_threshold
    if (0.0 != d.ob_imbalance_threshold_delta)
    {
        events.push_back(apply_one("ob_imbalance_threshold", d.ob_imbalance_threshold_delta,
                                   bounds_.at("ob_imbalance_threshold"), params.ob_imbalance_threshold));
    }

    // 8. cooldown_seconds
    if (0.0 != d.cooldown_seconds_delta)
    {
        events.push_back(apply_one("cooldown_seconds", d.cooldown_seconds_delta,
                                   bounds_.at("cooldown_seconds"), params.cooldown_seconds));
    }

    // 9. stop_loss_pct
    if (0.0 != d.stop_loss_pct_delta)
    {
        events.push_back(apply_one("stop_loss_pct", d.stop_loss_pct_delta,
                                   bounds_.at("stop_loss_pct"), params.stop_loss_pct));
    }

    // 10. take_profit_pct
    if (0.0 != d.take_profit_pct_delta)
    {
        events.push_back(apply_one("take_profit_pct", d.take_profit_pct_delta,
                                   bounds_.at("take_profit_pct"), params.take_profit_pct));
    }

    PULSE_LOG_INFO("ai", "ParamAdvisor applied {}/10 parameter deltas", events.size());
    return events;
}

// ---------------------------------------------------------------------------
// apply_one — apply a single delta to a single atomic parameter field
//
// Steps:
//   1. Load the current value (acquire ordering — see the latest writes)
//   2. Clamp delta to ±bound.max_delta (prevent extreme per-cycle changes)
//   3. Compute new_value = old_value + clamped_delta
//   4. Clamp new_value to [bound.hard_min, bound.hard_max] (absolute safety)
//   5. Store new_value (release ordering — make visible to strategy threads)
//   6. Log a warning if either clamping occurred (observability)
//   7. Return OnParamUpdate event for downstream consumers
// ---------------------------------------------------------------------------
heartbeat::OnParamUpdate ParamAdvisor::apply_one(
        const std::string &name,
        double delta,
        const ParamBound &bound,
        std::atomic<double> &field)
{
    // 1. Load current value
    const double old_value = field.load(std::memory_order_acquire);

    // 2. Clamp delta to ±max_delta
    double clamped_delta = std::clamp(delta, -bound.max_delta, bound.max_delta);
    if (std::abs(clamped_delta - delta) > 1e-12)
    {
        PULSE_LOG_WARN("ai", "ParamAdvisor: {} delta clamped from {:.6f} to ±{:.6f}",
                name, delta, bound.max_delta);
    }

    // 3. Compute new value
    double new_value = old_value + clamped_delta;

    // 4. Clamp to hard bounds
    const double clamped_value = std::clamp(new_value, bound.hard_min, bound.hard_max);
    if (std::abs(clamped_value - new_value) > 1e-12)
    {
        PULSE_LOG_WARN("ai", "ParamAdvisor: {} value clamped from {:.6f} to [{:.6f}, {:.6f}]",
                name, new_value, bound.hard_min, bound.hard_max);
    }
    new_value = clamped_value;

    // 5. Store atomically
    field.store(new_value, std::memory_order_release);

    PULSE_LOG_INFO("ai", "ParamAdvisor: {} updated {:.6f} → {:.6f} (delta={:.6f})",
            name, old_value, new_value, clamped_delta);

    // 6. Return event
    return heartbeat::OnParamUpdate{name, old_value, new_value};
}

// ---------------------------------------------------------------------------
// bounds() — read-only access to the bounds map (for testing / inspection)
// ---------------------------------------------------------------------------
const std::unordered_map<std::string, ParamBound> &ParamAdvisor::bounds() const
{
    return bounds_;
}

// ---------------------------------------------------------------------------
// set_bound() — modify safety bounds at runtime
//
// Replaces (or creates) the bound for the given parameter name.
// Should not be called concurrently with apply() — typically called during
// configuration loading or from the WebUI parameter tuning interface.
// ---------------------------------------------------------------------------
void ParamAdvisor::set_bound(const std::string &param_name, const ParamBound &bound)
{
    bounds_[param_name] = bound;
    PULSE_LOG_INFO("ai", "ParamAdvisor: bound for '{}' updated (max_delta={}, hard=[{}, {}])",
            param_name, bound.max_delta, bound.hard_min, bound.hard_max);
}

} // namespace pulse::ai
