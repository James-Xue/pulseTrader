#pragma once
// stop_loss_engine.hpp — Stop-loss evaluator (Layer 7 Risk Management)
//
// Three stop modes per position — fixed, trailing, time-based. Monitors open
// positions and signals when a stop should trigger.
//
// Design note: StopLossEngine does NOT execute close orders directly. It
// evaluates positions and returns stop signals. The caller (strategy or risk
// manager) is responsible for placing the close order. This keeps the engine
// pure and testable.
//
// Thread safety:
//   - std::shared_mutex for the tracked positions map
//   - register/remove take exclusive lock
//   - evaluate takes exclusive lock (updates trailing best_price)

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
// StopLossEngine — evaluates positions against stop conditions
// ---------------------------------------------------------------------------
class StopLossEngine
{
  public:
    /// Construct with default stop-loss configuration.
    explicit StopLossEngine(const StopLossConfig &default_config);

    // --- Position registration ---

    /// Register a position for stop-loss monitoring.
    /// Uses default_config if config parameter has zero/empty values.
    void registerPosition(const std::string &position_id, const Position &pos,
        const StopLossConfig &config = StopLossConfig{ StopMode::Fixed, 0.0, 0.0, 0 });

    /// Stop monitoring a position (position was closed).
    void removePosition(const std::string &position_id);

    // --- Evaluation ---

    /// Evaluate all tracked positions against their stop conditions.
    ///
    /// For each tracked position:
    ///   1. Look up current position in current_positions; skip if not found
    ///   2. Update trailing best_price if applicable
    ///   3. Check stop condition based on mode:
    ///      - Fixed:      Buy: current <= entry * (1 - fixed_pct)
    ///                    Sell: current >= entry * (1 + fixed_pct)
    ///      - Trailing:   Buy: current <= best * (1 - trailing_pct)
    ///                    Sell: current >= best * (1 + trailing_pct)
    ///      - TimeBased:  elapsed >= max_hold_seconds
    ///   4. Add triggered position_ids to result
    ///
    /// Returns a vector of position_ids that should be closed.
    [[nodiscard]] std::vector<std::string> evaluate(
        const std::unordered_map<std::string, Position> &current_positions,
        Timestamp current_time);

    // --- Queries ---
    [[nodiscard]] bool isTracked(const std::string &position_id) const;
    [[nodiscard]] std::size_t trackedCount() const;

  private:
    StopLossConfig m_defaultConfig;
    mutable std::shared_mutex m_mutex;

    /// Internal tracking state per position.
    struct TrackedStop
    {
        StopLossConfig config;
        Price best_price;       ///< Best price observed (for trailing stop).
        Timestamp open_time;    ///< When position was opened (for time-based stop).
        Side side;              ///< Position direction.
        Price entry_price;      ///< Entry price (for fixed stop).
    };

    std::unordered_map<std::string, TrackedStop> m_tracked;
};

} // namespace pulse::risk
