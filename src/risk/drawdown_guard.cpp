// drawdownGuard.cpp — Rolling PnL monitor + circuit breaker (Layer 7 Risk Management)

#include "risk/drawdown_guard.hpp"

#include "logging/logger.hpp"

namespace pulse::risk
{

using namespace pulse::logging;

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
    // Update equity and check drawdown thresholds:
    // 1. Acquire exclusive lock
    // 2. Update m_currentEquity
    // 3. If current > peak: update m_peakEquity
    // 4. If daily_start is zero, initialize it to current equity
    // 5. Compute daily drawdown: (daily_start - current) / daily_start
    // 6. If daily drawdown > threshold: set halt flag
    // 7. Compute peak-to-valley: (peak - current) / peak
    // 8. If peak-to-valley drawdown > threshold: set halt flag

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

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

} // namespace pulse::risk
