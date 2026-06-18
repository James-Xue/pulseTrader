#pragma once
// drawdown_guard.hpp — Rolling PnL monitor + circuit breaker (Layer 7 Risk Management)
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
#include "core/error.hpp"

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
    ///   1. daily_pnl_ accumulator
    ///   2. Per-strategy PnL map (if strategy_id is non-empty)
    void record_pnl(double pnl, const std::string &strategy_id = "");

    /// Set current equity value (called periodically from portfolio snapshot).
    ///
    /// Updates:
    ///   1. current_equity_
    ///   2. peak_equity_ (if new high)
    ///   3. Check daily drawdown: (daily_start - current) / daily_start
    ///   4. Check peak-to-valley: (peak - current) / peak
    ///   5. Set halt flag if either threshold is breached
    void update_equity(double equity);

    // --- Circuit breaker queries ---

    /// Returns true if trading is halted (any threshold breached).
    /// Lock-free: uses atomic<bool> for hot-path reads.
    [[nodiscard]] bool is_halted() const noexcept;

    /// Returns the specific reason for halt, or ErrorCode::Ok if not halted.
    [[nodiscard]] ErrorCode halt_reason() const noexcept;

    /// Current daily drawdown as a fraction of starting equity.
    /// Returns 0.0 if daily_start_equity_ is zero or current >= daily start.
    [[nodiscard]] double daily_drawdown() const;

    /// Current peak-to-valley drawdown as a fraction of peak equity.
    /// Returns 0.0 if peak_equity_ is zero or current >= peak.
    [[nodiscard]] double max_drawdown() const;

    // --- Lifecycle ---

    /// Reset daily counters (call at midnight UTC or session start).
    /// Sets daily_start_equity_ to current_equity_ and resets daily_pnl_.
    void reset_daily();

    /// Clear halt state (manual override after investigation).
    /// Resets halted_ flag and halt_reason_ to Ok.
    void clear_halt();

  private:
    RiskConfig config_;
    mutable std::shared_mutex mutex_;

    double peak_equity_{ 0.0 };         ///< Highest equity observed.
    double daily_start_equity_{ 0.0 };  ///< Equity at start of trading day.
    double current_equity_{ 0.0 };      ///< Latest equity snapshot.
    double daily_pnl_{ 0.0 };           ///< Cumulative realized PnL today.

    std::atomic<bool> halted_{ false };                ///< Circuit breaker flag (lock-free read).
    std::atomic<ErrorCode> halt_reason_{ ErrorCode::Ok }; ///< Reason for halt (lock-free read).

    std::unordered_map<std::string, double> strategy_pnl_; ///< Per-strategy realized PnL.
};

} // namespace pulse::risk
