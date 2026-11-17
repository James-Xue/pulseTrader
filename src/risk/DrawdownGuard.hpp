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
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// Drawdown equity — the account legs that feed the circuit breaker
// ---------------------------------------------------------------------------
//
// The breaker answers one question: how much *trading* equity can the engine
// still lose? Only accounts that are marked to market in the same snapshot may
// answer it.
//
// The spot USDT cash balance is deliberately NOT an input. A spot purchase
// converts USDT into a token: the cash leg collapses by the full position size
// while the token's value never enters this snapshot, so every spot entry reads
// as a total loss. That is not hypothetical — on 2026-11-17 the spot cash leg
// read 500.00 and then fell back to 0.00 against a 19.45 USDT futures wallet,
// producing a 90.9% "drawdown" with zero open positions. The resulting halt
// latched and blocked every entry (PT strategies and the GT external runner
// alike) for 20 hours until the engine was restarted.
struct DrawdownEquityInputs
{
    std::optional<double> futures_total; ///< Futures wallet total (USDT).
    std::optional<double> cfd_equity;    ///< CFD account equity (USD ≈ USDT).
};

struct DrawdownEquity
{
    double total = 0.0;    ///< Sum of every leg that was fetched.
    bool complete = false; ///< True only when EVERY leg fetched successfully.
};

/// Aggregate the mark-to-market account legs for the drawdown breaker.
///
/// A leg that failed to fetch is absent (std::nullopt) and makes the result
/// incomplete. Callers must skip the guard update in that case: feeding a
/// partial sum after a transient REST failure fakes a huge drawdown and halts
/// trading for no reason.
///
/// Thread safety: pure function, no shared state.
[[nodiscard]] DrawdownEquity computeDrawdownEquity(const DrawdownEquityInputs &inputs);

// ---------------------------------------------------------------------------
// DrawdownGuard — circuit breaker that halts trading on excessive drawdown
// ---------------------------------------------------------------------------
class DrawdownGuard
{
  public:
    /// Construct with risk configuration containing drawdown thresholds.
    explicit DrawdownGuard(const RiskConfig &config);

    // --- Snapshot plausibility (see updateEquity) ---

    /// Largest upward equity step a single snapshot may carry. A bigger jump
    /// is either a bad read or an external deposit; believing it immediately
    /// arms the peak with a level the account never held, and the next normal
    /// snapshot then reads as a catastrophic drawdown.
    static constexpr double kMaxPeakStepFraction = 0.50;

    /// Consecutive snapshots required before an outsized high is accepted as a
    /// real balance (a deposit) instead of a one-off bad read.
    static constexpr int kPeakStepConfirmations = 2;

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
    ///   0. Rejects the snapshot outright when it jumps more than
    ///      kMaxPeakStepFraction above the peak and does not repeat — an
    ///      implausible read must never touch a baseline. A repeated jump is
    ///      accepted as an external cash flow and rebases both baselines
    ///   1. Rolls the daily baseline when the UTC day (days since epoch) changes
    ///   2. m_currentEquity
    ///   3. m_peakEquity (if new high)
    ///   4. Check daily drawdown: (daily_start - current) / daily_start
    ///   5. Check peak-to-valley: (peak - current) / peak
    ///   6. Set halt flag if either threshold is breached
    void updateEquity(double equity);

    /// Deterministic overload used by tests: the caller supplies the UTC day
    /// number (days since 1970-01-01) instead of reading the system clock.
    void updateEquity(double equity, std::int64_t utc_day);

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

    /// Manually halt trading (circuit-breaker override via CLI/MCP).
    ///
    /// Sets the halt flag without touching PnL/equity bookkeeping —
    /// a pure operator override. Resume with clearHalt().
    ///
    /// Parameters:
    ///   1. reason — halt reason (default: ErrorCode::ManualHalt)
    void manualHalt(ErrorCode reason = ErrorCode::ManualHalt);

  private:
    /// Decide whether a snapshot may touch any baseline. Caller holds m_mutex
    /// exclusively.
    ///
    /// Returns false when the reading is an unconfirmed upward jump — the
    /// caller then drops the snapshot entirely instead of clamping it, so no
    /// baseline is ever armed with a level the account did not actually hold.
    /// A jump that repeats within the confirmation band is treated as an
    /// external cash flow: both baselines rebase to it and it returns true.
    [[nodiscard]] bool acceptSnapshotLocked(double equity);

    /// Forget any pending (unconfirmed) high/low run. Caller holds m_mutex.
    void clearPendingSteps() noexcept;

    RiskConfig m_config;
    mutable std::shared_mutex m_mutex;

    double m_peakEquity{ 0.0 };         ///< Highest equity observed.
    double m_dailyStartEquity{ 0.0 };  ///< Equity at start of trading day.
    double m_currentEquity{ 0.0 };      ///< Latest equity snapshot.
    double m_dailyPnl{ 0.0 };           ///< Cumulative realized PnL today.
    std::int64_t m_dayKey{ -1 };        ///< UTC day of the current daily baseline.
    double m_pendingHighEquity{ 0.0 };  ///< High from an unconfirmed jump.
    int m_pendingHighCount{ 0 };        ///< Consecutive snapshots at that high.
    double m_pendingLowEquity{ 0.0 };   ///< Low from an unconfirmed drop.
    int m_pendingLowCount{ 0 };         ///< Consecutive snapshots at that low.

    std::atomic<bool> m_halted{ false };                ///< Circuit breaker flag (lock-free read).
    std::atomic<ErrorCode> m_haltReason{ ErrorCode::Ok }; ///< Reason for halt (lock-free read).

    std::unordered_map<std::string, double> m_strategyPnl; ///< Per-strategy realized PnL.
};

} // namespace pulse::risk
