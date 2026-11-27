// drawdownGuard.cpp — Rolling PnL monitor + circuit breaker (Layer 7 Risk Management)

#include "risk/DrawdownGuard.hpp"

#include "logging/Logger.hpp"

#include <chrono>
#include <cmath>

namespace pulse::risk
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Drawdown equity aggregation
// ---------------------------------------------------------------------------

DrawdownEquity computeDrawdownEquity(const DrawdownEquityInputs &inputs)
{
    // Sum every mark-to-market leg that was fetched:
    // 1. A leg that failed to fetch is std::nullopt — the whole snapshot is
    //    incomplete, because a partial sum reads as an instant loss
    // 2. A leg that parsed to a non-finite value is treated as a failed fetch:
    //    std::from_chars accepts "nan"/"inf", and letting one through would
    //    poison every later drawdown ratio (see acceptSnapshotLocked)
    // 3. Every present leg contributes; USD and USDT are treated 1:1
    //
    // Spot cash is intentionally absent from DrawdownEquityInputs — see the
    // header for the 2026-11-17 incident that made that exclusion necessary.
    DrawdownEquity out;
    out.complete = true;

    if (!inputs.futures_total.has_value() || !std::isfinite(*inputs.futures_total))
    {
        out.complete = false;
    }
    else
    {
        out.total += *inputs.futures_total;
    }

    if (!inputs.cfd_equity.has_value() || !std::isfinite(*inputs.cfd_equity))
    {
        out.complete = false;
    }
    else
    {
        out.total += *inputs.cfd_equity;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DrawdownGuard::DrawdownGuard(const RiskConfig &config)
    : m_config{ config }
{
}

// ---------------------------------------------------------------------------
// PnL reporting
// ---------------------------------------------------------------------------

void DrawdownGuard::recordPnl(double pnl, const std::string &strategy_id)
{
    // Record a realized PnL event:
    // 1. Acquire exclusive lock (modifying shared state)
    // 2. Accumulate m_dailyPnl
    // 3. If strategy_id is non-empty, accumulate per-strategy PnL
    // 4. Log the event

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    m_dailyPnl += pnl;

    if (!strategy_id.empty())
    {
        m_strategyPnl[strategy_id] += pnl;
    }

    PULSE_LOG_DEBUG("risk",
        "Recorded PnL: {:.4f} (strategy: {}), daily total: {:.4f}",
        pnl, strategy_id.empty() ? "aggregate" : strategy_id, m_dailyPnl);
}

void DrawdownGuard::updateEquity(double equity)
{
    // UTC day number (days since 1970-01-01). Integer hours/24 keeps the
    // boundary DST-free and timezone-independent — the trading day is UTC.
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto utc_day = std::chrono::duration_cast<std::chrono::hours>(since_epoch).count() / 24;
    updateEquity(equity, static_cast<std::int64_t>(utc_day));
}

void DrawdownGuard::seedEquity(double peak_equity, double daily_start_equity)
{
    // Restore the persisted baselines (paper mode restart):
    // 1. Reject non-finite/non-positive values — they are not balances
    // 2. Restore peak and daily-start, with the current equity at the daily
    //    start until the first fresh snapshot arrives
    // 3. Align the day key so the next update does not roll the baseline
    // 4. Clear any pending plausibility steps from a previous process
    if (!std::isfinite(peak_equity) || peak_equity <= 0.0
        || !std::isfinite(daily_start_equity) || daily_start_equity <= 0.0)
    {
        PULSE_LOG_WARN("risk",
            "Ignoring invalid drawdown seed (peak {:.2f}, daily {:.2f})",
            peak_equity, daily_start_equity);
        return;
    }

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    m_peakEquity = peak_equity;
    m_dailyStartEquity = daily_start_equity;
    m_currentEquity = daily_start_equity;
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    m_dayKey = std::chrono::duration_cast<std::chrono::hours>(since_epoch).count() / 24;
    clearPendingSteps();

    PULSE_LOG_INFO("risk",
        "Seeded drawdown baselines — peak {:.2f}, daily start {:.2f}",
        m_peakEquity, m_dailyStartEquity);
}

bool DrawdownGuard::acceptSnapshotLocked(double equity)
{
    // Classify one equity snapshot before it can move any baseline.
    //
    // The failure this guards against (2026-11-17): a single snapshot reported
    // an equity 11x the real account. Accepting it as the peak armed the
    // breaker at 519.45 USDT; the next snapshot returned to 47.16 and the
    // breaker read a 90.9% peak-to-valley drawdown with zero open positions,
    // then latched a halt no later snapshot could clear.
    //
    // Rules:
    // 1. A non-finite reading is never a balance — the REST parsers accept
    //    "nan"/"inf" through std::from_chars. Letting one through poisons the
    //    ratios permanently: every later comparison is NaN and NaN > threshold
    //    is always false, so the limit silently dies
    // 2. A non-positive reading is either a depleted account (halt) or noise on
    //    a guard with no baseline yet (drop it) — it never becomes a baseline,
    //    because the day/PnL ratios are only defined for a positive base
    // 3. The first real snapshot has nothing to compare against — accept it
    // 4. A step within kMaxPeakStepFraction is ordinary growth or loss — accept
    // 5. A larger UPWARD step is unconfirmed: drop it, but remember it
    // 6. The same elevated level seen kPeakStepConfirmations times running is a
    //    real balance change (a deposit): rebase both baselines to it, so the
    //    cash flow is never mistaken for performance
    // 7. A larger DOWNWARD step gets exactly ONE grace snapshot: a cash
    //    transfer out, or a bad read, is far likelier in 80 s than a real
    //    mark-to-market loss of that size. It is NOT rebased — a level that
    //    persists is accepted on the next snapshot and halts normally, so a
    //    genuine crash is delayed by one heartbeat, never forgiven

    if (!std::isfinite(equity))
    {
        PULSE_LOG_WARN("risk",
            "Ignoring non-finite equity snapshot ({:.2f}) — baseline unchanged", equity);
        return false;
    }

    if (equity <= 0.0)
    {
        if (m_peakEquity > 0.0)
        {
            PULSE_LOG_WARN("risk",
                "Equity {:.2f} is not positive — halting (account depleted)", equity);
            m_halted.store(true, std::memory_order_release);
            m_haltReason.store(ErrorCode::DrawdownLimitHit, std::memory_order_release);
        }
        else
        {
            PULSE_LOG_WARN("risk",
                "Ignoring non-positive equity snapshot ({:.2f}) before any baseline",
                equity);
        }
        return false;
    }

    if (m_peakEquity <= 0.0)
    {
        clearPendingSteps();
        return true;
    }

    const double upper = m_peakEquity * (1.0 + kMaxPeakStepFraction);
    const double lower = m_peakEquity * (1.0 - kMaxPeakStepFraction);

    if (equity >= lower && equity <= upper)
    {
        clearPendingSteps();
        return true;
    }

    if (equity > upper)
    {
        // Same elevated level as last time (within a 2% band)? Count it; a
        // different value restarts the confirmation run, so two unrelated
        // bad reads cannot confirm each other.
        const double band = std::abs(m_pendingHighEquity) * 0.02;
        if (m_pendingHighCount > 0 && std::abs(equity - m_pendingHighEquity) <= band)
        {
            ++m_pendingHighCount;
        }
        else
        {
            m_pendingHighEquity = equity;
            m_pendingHighCount = 1;
        }

        if (m_pendingHighCount < kPeakStepConfirmations)
        {
            PULSE_LOG_WARN("risk",
                "Ignoring implausible equity snapshot {:.2f} vs peak {:.2f} "
                "({:+.1f}%) — awaiting confirmation ({}/{})",
                equity, m_peakEquity,
                (equity / m_peakEquity - 1.0) * 100.0,
                m_pendingHighCount, kPeakStepConfirmations);
            return false;
        }

        PULSE_LOG_WARN("risk",
            "Confirmed equity step {:.2f} -> {:.2f} ({:+.1f}%) — rebasing peak and "
            "daily baseline (treated as an external cash flow)",
            m_peakEquity, equity, (equity / m_peakEquity - 1.0) * 100.0);

        m_peakEquity = equity;
        m_dailyStartEquity = equity;
        clearPendingSteps();
        return true;
    }

    // Downward grace: any second consecutive snapshot below `lower` confirms,
    // whatever its exact level — a real drop keeps moving, and demanding that
    // two readings agree would let a fast crash dodge confirmation forever.
    if (m_pendingLowCount > 0)
    {
        ++m_pendingLowCount;
    }
    else
    {
        m_pendingLowCount = 1;
    }
    m_pendingLowEquity = equity;

    if (m_pendingLowCount < kPeakStepConfirmations)
    {
        PULSE_LOG_WARN("risk",
            "Ignoring implausible equity drop {:.2f} vs peak {:.2f} ({:+.1f}%) — "
            "one grace snapshot before the breaker acts",
            equity, m_peakEquity,
            (equity / m_peakEquity - 1.0) * 100.0);
        return false;
    }

    PULSE_LOG_WARN("risk",
        "Confirmed equity drop {:.2f} -> {:.2f} ({:+.1f}%) — evaluating drawdown",
        m_peakEquity, equity, (equity / m_peakEquity - 1.0) * 100.0);

    clearPendingSteps();
    return true;
}

void DrawdownGuard::clearPendingSteps() noexcept
{
    m_pendingHighEquity = 0.0;
    m_pendingHighCount = 0;
    m_pendingLowEquity = 0.0;
    m_pendingLowCount = 0;
}


void DrawdownGuard::updateEquity(double equity, std::int64_t utc_day)
{
    // Update equity and check drawdown thresholds:
    // 0. Acquire exclusive lock
    // 1. Reject an implausible upward jump before it touches any baseline
    // 2. Roll the daily baseline when the UTC day changes (otherwise the
    //    "daily" limit silently becomes an "since process start" limit)
    // 3. Update m_currentEquity
    // 4. If current > peak: update m_peakEquity
    // 5. If daily_start is zero, initialize it to current equity
    // 6. Compute daily drawdown: (daily_start - current) / daily_start
    // 7. If daily drawdown > threshold: set halt flag
    // 8. Compute peak-to-valley: (peak - current) / peak
    // 9. If peak-to-valley drawdown > threshold: set halt flag

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    // 1. Plausibility gate. Dropping the snapshot outright (rather than
    //    clamping it) is what keeps the peak and the daily baseline free of a
    //    level the account never actually held.
    if (!acceptSnapshotLocked(equity))
    {
        return;
    }

    // 2. New UTC day → rebase the daily start on the first snapshot of the day.
    if (m_dayKey != utc_day)
    {
        m_dayKey = utc_day;
        m_dailyStartEquity = equity;
        m_dailyPnl = 0.0;
        m_strategyPnl.clear();

        PULSE_LOG_INFO("risk",
            "New UTC day {} — daily baseline reset to {:.2f}", utc_day, equity);
    }

    m_currentEquity = equity;

    // 3. Update peak if new high.
    if (m_currentEquity > m_peakEquity)
    {
        m_peakEquity = m_currentEquity;
    }

    // 4. Initialize daily start if first call.
    if (0.0 == m_dailyStartEquity)
    {
        m_dailyStartEquity = m_currentEquity;
    }

    // 5. Check daily drawdown.
    if (m_dailyStartEquity > 0.0)
    {
        const double daily_dd = (m_dailyStartEquity - m_currentEquity) / m_dailyStartEquity;

        if (daily_dd > m_config.maxDailyDrawdown)
        {
            PULSE_LOG_WARN("risk",
                "Daily drawdown {:.4f} exceeds threshold {:.4f} - halting trading",
                daily_dd, m_config.maxDailyDrawdown);
            m_halted.store(true, std::memory_order_release);
            m_haltReason.store(ErrorCode::DrawdownLimitHit, std::memory_order_release);
        }
    }

    // 7. Check peak-to-valley drawdown.
    if (m_peakEquity > 0.0)
    {
        const double peak_dd = (m_peakEquity - m_currentEquity) / m_peakEquity;

        if (peak_dd > m_config.maxDrawdown)
        {
            PULSE_LOG_WARN("risk",
                "Peak-to-valley drawdown {:.4f} exceeds threshold {:.4f} - halting trading",
                peak_dd, m_config.maxDrawdown);
            m_halted.store(true, std::memory_order_release);
            m_haltReason.store(ErrorCode::DrawdownLimitHit, std::memory_order_release);
        }
    }
}

// ---------------------------------------------------------------------------
// Circuit breaker queries
// ---------------------------------------------------------------------------

bool DrawdownGuard::isHalted() const noexcept
{
    return m_halted.load(std::memory_order_acquire);
}

ErrorCode DrawdownGuard::haltReason() const noexcept
{
    return m_haltReason.load(std::memory_order_acquire);
}

double DrawdownGuard::dailyDrawdown() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    if (m_dailyStartEquity <= 0.0)
    {
        return 0.0;
    }

    const double dd = (m_dailyStartEquity - m_currentEquity) / m_dailyStartEquity;
    return (dd > 0.0) ? dd : 0.0;
}

double DrawdownGuard::maxDrawdown() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    if (m_peakEquity <= 0.0)
    {
        return 0.0;
    }

    const double dd = (m_peakEquity - m_currentEquity) / m_peakEquity;
    return (dd > 0.0) ? dd : 0.0;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DrawdownGuard::resetDaily()
{
    // Reset daily counters for a new trading day:
    // 1. Acquire exclusive lock
    // 2. Set m_dailyStartEquity to m_currentEquity
    // 3. Reset m_dailyPnl to zero
    // 4. Clear per-strategy PnL map

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    // Keep the day key in sync so the next updateEquity() does not roll again
    // (which would be harmless but would log a spurious baseline reset).
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    m_dayKey = std::chrono::duration_cast<std::chrono::hours>(since_epoch).count() / 24;

    m_dailyStartEquity = m_currentEquity;
    m_dailyPnl = 0.0;
    m_strategyPnl.clear();

    PULSE_LOG_INFO("risk",
        "Daily counters reset — start equity: {:.2f}", m_dailyStartEquity);
}

void DrawdownGuard::clearHalt()
{
    m_halted.store(false, std::memory_order_release);
    m_haltReason.store(ErrorCode::Ok, std::memory_order_release);

    PULSE_LOG_INFO("risk", "Trading halt cleared manually");
}

void DrawdownGuard::manualHalt(ErrorCode reason)
{
    m_halted.store(true, std::memory_order_release);
    m_haltReason.store(reason, std::memory_order_release);

    PULSE_LOG_WARN("risk", "Trading halted manually (reason={})",
                   static_cast<int>(reason));
}

} // namespace pulse::risk
