#pragma once
// drawdownGuard.hpp — Rolling PnL monitor + circuit breaker (Layer 7 Risk Management)
//
// Monitors rolling PnL (per strategy and aggregate) and automatically halts
// new order signals when drawdown thresholds are breached:
//   1. Daily drawdown  — loss relative to the start-of-day equity
//   2. Peak-to-valley  — decline from the highest equity ever observed
//
// Thread safety:
//   - std::shared_mutex for peak equity and daily PnL state (write path)
//   - std::atomic<bool> for the halt flag (hot-path readers check lock-free)
//   - std::atomic<ErrorCode> for halt reason (lock-free read)

#include "core/config.hpp"
#include "core/PulseError.hpp"

#include <atomic>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// DrawdownGuard — circuit breaker that halts trading on excessive drawdown
// ---------------------------------------------------------------------------
class DrawdownGuard
{
  public:
    /// Construct with risk configuration containing drawdown thresholds.
    explicit DrawdownGuard(const RiskConfig &config);

    // --- PnL reporting ---

    /// Record a realized PnL event (positive = profit, negative = loss).
    ///
    /// Updates:
    ///   1. m_dailyPnl accumulator
    ///   2. Per-strategy PnL map (if strategy_id is non-empty)
    void recordPnl(double pnl, const std::string &strategy_id = "");

    /// Set current equity value (called periodically from portfolio snapshot).
    ///
    /// Updates:
    ///   1. m_currentEquity
    ///   2. m_peakEquity (if new high)
    ///   3. Check daily drawdown: (daily_start - current) / daily_start
    ///   4. Check peak-to-valley: (peak - current) / peak
    ///   5. Set halt flag if either threshold is breached
    void updateEquity(double equity);

    // --- Circuit breaker queries ---

    /// Returns true if trading is halted (any threshold breached).
    /// Lock-free: uses atomic<bool> for hot-path reads.
    [[nodiscard]] bool isHalted() const noexcept;

    /// Returns the specific reason for halt, or ErrorCode::Ok if not halted.
    [[nodiscard]] ErrorCode haltReason() const noexcept;

    /// Current daily drawdown as a fraction of starting equity.
    /// Returns 0.0 if m_dailyStartEquity is zero or current >= daily start.
    [[nodiscard]] double dailyDrawdown() const;

    /// Current peak-to-valley drawdown as a fraction of peak equity.
    /// Returns 0.0 if m_peakEquity is zero or current >= peak.
    [[nodiscard]] double maxDrawdown() const;

    // --- Lifecycle ---

    /// Reset daily counters (call at midnight UTC or session start).
    /// Sets m_dailyStartEquity to m_currentEquity and resets m_dailyPnl.
    void resetDaily();

    /// Clear halt state (manual override after investigation).
    /// Resets m_halted flag and m_haltReason to Ok.
    void clearHalt();

  private:
    RiskConfig m_config;
    mutable std::shared_mutex m_mutex;

    double m_peakEquity{ 0.0 };         ///< Highest equity observed.
    double m_dailyStartEquity{ 0.0 };  ///< Equity at start of trading day.
    double m_currentEquity{ 0.0 };      ///< Latest equity snapshot.
    double m_dailyPnl{ 0.0 };           ///< Cumulative realized PnL today.

    std::atomic<bool> m_halted{ false };                ///< Circuit breaker flag (lock-free read).
    std::atomic<ErrorCode> m_haltReason{ ErrorCode::Ok }; ///< Reason for halt (lock-free read).

    std::unordered_map<std::string, double> m_strategyPnl; ///< Per-strategy realized PnL.
};

} // namespace pulse::risk
