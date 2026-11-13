#pragma once
// ProtectiveExitManager.hpp — engine-side stop-loss / take-profit supervisor
//
// Layer 7 (Risk Management). One evaluation pass per main-loop tick:
//   1. Snapshot every open position and refresh its mark price from the feeds
//   2. Register new positions / drop closed ones with the two evaluators
//   3. Ask StopLossEngine which positions breached a stop → close the whole
//      remaining quantity
//   4. Ask TakeProfitEngine which ladder targets were hit → close a fraction
//
// Why this exists: futures entries do not yet carry exchange-side SL/TP
// (audit B1, 2026-11-13), so this is the engine-side backstop. It does NOT
// replace exchange-side stops: if the process dies, only orders living on the
// exchange survive. The two layers are complementary and both are needed.
//
// Thread safety:
//   - Single-threaded: call tick() only from the main loop.
//   - PositionManager / the two engines are individually thread-safe, but the
//     registration set and the retry cooldown map are not synchronized.
//
// Failure handling:
//   - A failed close is retried after kRetryCooldownSeconds, so a transient
//     REST error does not spam the exchange on every tick.
//   - A missing quote keeps the previous mark price — a data gap must not
//     fabricate a stop trigger.

#include "core/config.hpp"
#include "core/types.hpp"
#include "risk/PositionManager.hpp"
#include "risk/StopLossEngine.hpp"
#include "risk/TakeProfitEngine.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// ProtectiveExitManager — evaluates and fires protective exits
// ---------------------------------------------------------------------------
class ProtectiveExitManager
{
  public:
    /// Resolve the latest price for a symbol on a market. Return nullopt when
    /// no fresh tick is available — the position keeps its previous mark.
    using PriceLookup = std::function<std::optional<Price>(const Symbol &symbol,
                                                           MarketType market_type)>;

    /// Request a reduce-only close of `quantity` from `pos`.
    ///
    /// Returns true when the request was accepted (order placed / close
    /// endpoint called) and false on failure, which schedules a retry.
    using CloseFn = std::function<bool(const Position &pos, double quantity)>;

    ProtectiveExitManager(const RiskConfig &config,
                          PositionManager &positions,
                          PriceLookup price_lookup,
                          CloseFn close);

    /// Outcome counters for one tick (used for logging and tests).
    struct TickResult
    {
        int stopped{ 0 };
        int took_profit{ 0 };
        int failed{ 0 };
    };

    /// Run one evaluation pass. Call from the main loop.
    ///
    /// Parameters:
    ///   now — current wall-clock time (system_clock), used for time-based
    ///         stops and for the per-position retry cooldown.
    TickResult tick(Timestamp now);

    /// Number of positions currently registered with the evaluators.
    [[nodiscard]] std::size_t trackedPositions() const;

    /// Minimum seconds between two close attempts for the same position.
    static constexpr std::uint32_t kRetryCooldownSeconds = 10;

  private:
    /// True when no close attempt for this position happened within the
    /// cooldown window.
    [[nodiscard]] bool canFire(const std::string &position_id, Timestamp now) const;

    /// Record a close attempt for the cooldown check.
    void markFired(const std::string &position_id, Timestamp now);

    RiskConfig m_config;
    PositionManager &m_positions;
    PriceLookup m_priceLookup;
    CloseFn m_close;

    StopLossEngine m_stops;
    TakeProfitEngine m_takeProfits;

    std::unordered_set<std::string> m_registered;          ///< Position ids being monitored.
    std::unordered_map<std::string, Timestamp> m_lastAttempt; ///< Retry cooldown bookkeeping.
};

} // namespace pulse::risk