#pragma once
// paramAdvisor.hpp — Delta validation + atomic StrategyParams writes (Layer 4 AI Analysis)
//
// Receives an AnalysisResult from AIClient and applies the recommended parameter
// deltas to a StrategyParams instance atomically. Every delta is validated against
// configurable safety bounds before being applied:
//   1. Delta is clamped to ±max_delta (prevents the LLM from making extreme changes)
//   2. Resulting value is clamped to [hard_min, hard_max] (absolute safety limits)
//   3. The clamped value is written to the atomic parameter field
//   4. An OnParamUpdate event is emitted for each changed parameter (observability)
//
// Design rationale:
//   - Safety bounds prevent the LLM from setting dangerous parameter values
//     (e.g., order_quantity = 100 BTC, or stop_loss_pct = 0.0)
//   - Bounds are configurable at runtime for live tuning without recompilation
//   - Atomic writes ensure the strategy thread sees consistent parameter values
//   - OnParamUpdate events feed the heartbeat system for logging and WebUI display
//
// Thread safety:
//   - apply() writes to StrategyParams atomics (release ordering)
//   - Strategy threads read the same atomics (acquire ordering)
//   - m_bounds is not modified during apply() — safe for concurrent reads
//   - setBound() should only be called from the configuration thread (not concurrent with apply)

#include "ai/analysis_result.hpp"
#include "heartbeat/heartbeat_events.hpp"
#include "strategy/strategy_params.hpp"

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// ParamBound — safety constraints for a single tunable parameter
//
// Fields:
//   1. max_delta — maximum change per AI cycle (±); the LLM delta is clamped to this
//   2. hard_min  — absolute minimum value the parameter can reach (floor)
//   3. hard_max  — absolute maximum value the parameter can reach (ceiling)
//
// Example: order_quantity has max_delta=0.0005, hard_min=0.0001, hard_max=0.1
//   → The LLM can request ±0.0005 per cycle, but the final value can never
//     go below 0.0001 BTC or above 0.1 BTC.
// ---------------------------------------------------------------------------
struct ParamBound
{
    double max_delta;  ///< Max change per cycle (±).
    double hard_min;   ///< Absolute minimum value.
    double hard_max;   ///< Absolute maximum value.
};

// ---------------------------------------------------------------------------
// ParamAdvisor — validates and applies AI-recommended parameter deltas
//
// Usage:
//   ParamAdvisor advisor;  // default bounds
//   auto events = advisor.apply(analysis_result, strategy_params);
//   // events contains OnParamUpdate for each changed parameter
// ---------------------------------------------------------------------------
class ParamAdvisor
{
  public:
    /// Construct with default safety bounds for all 10 tunable parameters.
    ///
    /// Default bounds are conservative — designed to limit per-cycle changes
    /// while allowing the full useful range over multiple cycles.
    ParamAdvisor();

    /// Apply validated deltas to a strategy's params (atomic writes).
    ///
    /// Steps for each of the 10 parameter deltas:
    ///   1. Skip if delta is exactly zero (no change requested)
    ///   2. Look up the safety bounds for this parameter
    ///   3. Clamp the delta to ±max_delta
    ///   4. Compute new_value = current_value + clamped_delta
    ///   5. Clamp new_value to [hard_min, hard_max]
    ///   6. Atomically store the new value
    ///   7. Log if clamping occurred (for observability)
    ///   8. Emit an OnParamUpdate event
    ///
    /// Parameters:
    ///   1. result — the AI analysis result containing param_deltas
    ///   2. params — the strategy parameters to update (modified in place)
    ///
    /// Returns: vector of OnParamUpdate events (one per changed parameter).
    std::vector<heartbeat::OnParamUpdate> apply(
            const AnalysisResult &result,
            strategy::StrategyParams &params);

    /// Access the bounds map (read-only, for testing or inspection).
    [[nodiscard]] const std::unordered_map<std::string, ParamBound> &bounds() const;

    /// Modify the bounds for a specific parameter at runtime.
    ///
    /// If the parameter name already exists, its bound is replaced.
    /// If it does not exist, a new entry is created.
    ///
    /// This should not be called concurrently with apply().
    void setBound(const std::string &param_name, const ParamBound &bound);

  private:
    /// Apply a single delta to a single atomic parameter field.
    ///
    /// Steps:
    ///   1. Load the current value (acquire ordering)
    ///   2. Clamp delta to ±bound.max_delta
    ///   3. Compute new_value = old_value + clamped_delta
    ///   4. Clamp new_value to [bound.hard_min, bound.hard_max]
    ///   5. Store new_value (release ordering)
    ///   6. Log a warning if either clamping occurred
    ///   7. Return OnParamUpdate event with old and new values
    heartbeat::OnParamUpdate applyOne(
            const std::string &name,
            double delta,
            const ParamBound &bound,
            std::atomic<double> &field);

    /// Safety bounds map: parameter name → ParamBound.
    /// Populated in the constructor with defaults for all 10 AI-tunable parameters.
    std::unordered_map<std::string, ParamBound> m_bounds;
};

} // namespace pulse::ai
