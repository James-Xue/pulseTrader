// drawdown_guard.cpp — Rolling PnL monitor + circuit breaker (Layer 7 Risk Management)

#include "risk/drawdown_guard.hpp"

#include "logging/logger.hpp"

namespace pulse::risk
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DrawdownGuard::DrawdownGuard(const RiskConfig &config)
    : config_{ config }
{
}

// ---------------------------------------------------------------------------
// PnL reporting
// ---------------------------------------------------------------------------

void DrawdownGuard::record_pnl(double pnl, const std::string &strategy_id)
{
    // Record a realized PnL event:
    // 1. Acquire exclusive lock (modifying shared state)
    // 2. Accumulate daily_pnl_
    // 3. If strategy_id is non-empty, accumulate per-strategy PnL
    // 4. Log the event

    std::unique_lock<std::shared_mutex> write_lock(mutex_);

    daily_pnl_ += pnl;

    if (!strategy_id.empty())
    {
        strategy_pnl_[strategy_id] += pnl;
    }

    PULSE_LOG_DEBUG("risk",
        "Recorded PnL: {:.4f} (strategy: {}), daily total: {:.4f}",
        pnl, strategy_id.empty() ? "aggregate" : strategy_id, daily_pnl_);
}

void DrawdownGuard::update_equity(double equity)
{
    // Update equity and check drawdown thresholds:
    // 1. Acquire exclusive lock
    // 2. Update current_equity_
    // 3. If current > peak: update peak_equity_
    // 4. If daily_start is zero, initialize it to current equity
    // 5. Compute daily drawdown: (daily_start - current) / daily_start
    // 6. If daily drawdown > threshold: set halt flag
    // 7. Compute peak-to-valley: (peak - current) / peak
    // 8. If peak-to-valley drawdown > threshold: set halt flag

    std::unique_lock<std::shared_mutex> write_lock(mutex_);

    current_equity_ = equity;

    // 3. Update peak if new high.
    if (current_equity_ > peak_equity_)
    {
        peak_equity_ = current_equity_;
    }

    // 4. Initialize daily start if first call.
    if (0.0 == daily_start_equity_)
    {
        daily_start_equity_ = current_equity_;
    }

    // 5. Check daily drawdown.
    if (daily_start_equity_ > 0.0)
    {
        const double daily_dd = (daily_start_equity_ - current_equity_) / daily_start_equity_;

        if (daily_dd > config_.maxDailyDrawdown)
        {
            PULSE_LOG_WARN("risk",
                "Daily drawdown {:.4f} exceeds threshold {:.4f} - halting trading",
                daily_dd, config_.maxDailyDrawdown);
            halted_.store(true, std::memory_order_release);
            halt_reason_.store(ErrorCode::DrawdownLimitHit, std::memory_order_release);
        }
    }

    // 7. Check peak-to-valley drawdown.
    if (peak_equity_ > 0.0)
    {
        const double peak_dd = (peak_equity_ - current_equity_) / peak_equity_;

        if (peak_dd > config_.maxDrawdown)
        {
            PULSE_LOG_WARN("risk",
                "Peak-to-valley drawdown {:.4f} exceeds threshold {:.4f} - halting trading",
                peak_dd, config_.maxDrawdown);
            halted_.store(true, std::memory_order_release);
            halt_reason_.store(ErrorCode::DrawdownLimitHit, std::memory_order_release);
        }
    }
}

// ---------------------------------------------------------------------------
// Circuit breaker queries
// ---------------------------------------------------------------------------

bool DrawdownGuard::is_halted() const noexcept
{
    return halted_.load(std::memory_order_acquire);
}

ErrorCode DrawdownGuard::halt_reason() const noexcept
{
    return halt_reason_.load(std::memory_order_acquire);
}

double DrawdownGuard::daily_drawdown() const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);

    if (daily_start_equity_ <= 0.0)
    {
        return 0.0;
    }

    const double dd = (daily_start_equity_ - current_equity_) / daily_start_equity_;
    return (dd > 0.0) ? dd : 0.0;
}

double DrawdownGuard::max_drawdown() const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);

    if (peak_equity_ <= 0.0)
    {
        return 0.0;
    }

    const double dd = (peak_equity_ - current_equity_) / peak_equity_;
    return (dd > 0.0) ? dd : 0.0;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DrawdownGuard::reset_daily()
{
    // Reset daily counters for a new trading day:
    // 1. Acquire exclusive lock
    // 2. Set daily_start_equity_ to current_equity_
    // 3. Reset daily_pnl_ to zero
    // 4. Clear per-strategy PnL map

    std::unique_lock<std::shared_mutex> write_lock(mutex_);

    daily_start_equity_ = current_equity_;
    daily_pnl_ = 0.0;
    strategy_pnl_.clear();

    PULSE_LOG_INFO("risk",
        "Daily counters reset — start equity: {:.2f}", daily_start_equity_);
}

void DrawdownGuard::clear_halt()
{
    halted_.store(false, std::memory_order_release);
    halt_reason_.store(ErrorCode::Ok, std::memory_order_release);

    PULSE_LOG_INFO("risk", "Trading halt cleared manually");
}

} // namespace pulse::risk
