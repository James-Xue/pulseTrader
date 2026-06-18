#pragma once
// take_profit_engine.hpp — Partial take-profit ladder (Layer 7 Risk Management)
//
// Closes a configurable fraction of a position at each of N price targets,
// letting the remainder run until the stop is hit.
//
// Design note: Like StopLossEngine, this engine evaluates and signals but
// does not execute. It returns a vector of TpSignal structs containing
// position_id, close_fraction, and which target was hit.
//
// Thread safety:
//   - std::shared_mutex for the tracked positions map
//   - register/remove take exclusive lock
//   - evaluate takes exclusive lock (advances next_target_idx)

#include "core/config.hpp"
#include "core/types.hpp"
#include "risk/risk_types.hpp"

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// TakeProfitEngine — partial take-profit ladder evaluator
// ---------------------------------------------------------------------------
class TakeProfitEngine
{
  public:
    /// Construct with default take-profit configuration.
    explicit TakeProfitEngine(const TakeProfitConfig &default_config);

    // --- Position registration ---

    /// Register a position for take-profit monitoring.
    void register_position(const std::string &position_id, const Position &pos,
        const TakeProfitConfig &config = TakeProfitConfig{ true, {}, {} });

    /// Stop monitoring a position (position was closed).
    void remove_position(const std::string &position_id);

    // --- Evaluation ---

    /// Result of a take-profit evaluation for one position.
    struct TpSignal
    {
        std::string position_id;   ///< Which position triggered.
        double close_fraction;     ///< Fraction of remaining qty to close (0.0 to 1.0).
        int target_index;          ///< Which target level was hit.

        TpSignal()
            : position_id{}
            , close_fraction{ 0.0 }
            , target_index{ 0 }
        {
        }
    };

    /// Evaluate all tracked positions against their TP ladders.
    ///
    /// For each position:
    ///   1. Look up current position; skip if not found
    ///   2. Get next_target_idx; skip if all targets hit
    ///   3. Compute target price from entry +/- targets_pct
    ///   4. If price crossed target: emit TpSignal, advance index
    ///
    /// Returns all TpSignals generated this evaluation.
    [[nodiscard]] std::vector<TpSignal> evaluate(
        const std::unordered_map<std::string, Position> &current_positions);

    // --- Queries ---
    [[nodiscard]] bool is_tracked(const std::string &position_id) const;
    [[nodiscard]] std::size_t tracked_count() const;
    [[nodiscard]] int next_target_index(const std::string &position_id) const;

  private:
    TakeProfitConfig default_config_;
    mutable std::shared_mutex mutex_;

    /// Internal tracking state per position.
    struct TrackedTp
    {
        TakeProfitConfig config;
        Price entry_price;
        Side side;
        int next_target_idx; ///< Index of next target to check (advances as hit).
    };

    std::unordered_map<std::string, TrackedTp> tracked_;
};

} // namespace pulse::risk
