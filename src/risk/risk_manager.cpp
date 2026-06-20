// risk_manager.cpp — Central order gate (Layer 7 Risk Management)

#include "risk/risk_manager.hpp"

#include "logging/logger.hpp"

#include <cmath>

namespace pulse::risk
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RiskManager::RiskManager(const RiskConfig &config,
    PositionManager &position_manager,
    DrawdownGuard &drawdown_guard,
    OrderRateLimiter &rate_limiter)
    : config_{ config }
    , position_manager_{ position_manager }
    , drawdown_guard_{ drawdown_guard }
    , rate_limiter_{ rate_limiter }
{
}

// ---------------------------------------------------------------------------
// Order evaluation
// ---------------------------------------------------------------------------

RiskEvalResult RiskManager::evaluate_order(const execution::OrderRequest &order)
{
    // Evaluate a proposed order against all risk rules sequentially:
    // 1. Check DrawdownGuard: is trading halted?
    // 2. Check OrderRateLimiter: token bucket has capacity?
    // 3. Atomically check + reserve via PositionManager (TOCTOU-safe):
    //    - If full order fits: Approved
    //    - If partial order fits: Modified with reduced qty
    //    - If nothing fits: Rejected
    //
    // The atomic reserve_notional() call replaces the old 3-separate-lock
    // pattern (can_open_position + portfolio_summary + symbol_notional)
    // that was vulnerable to TOCTOU races under concurrent strategy threads.

    RiskEvalResult result;

    // 1. Check drawdown guard (atomic load — safe outside the reserve).
    if (drawdown_guard_.is_halted())
    {
        PULSE_LOG_WARN("risk",
            "Order rejected: trading halted (reason: {})",
            static_cast<int>(drawdown_guard_.halt_reason()));

        result.decision = RiskDecision::Rejected;
        result.approved_qty = 0.0;
        result.reason_code = drawdown_guard_.halt_reason();
        result.reason_message = "Trading halted: drawdown limit breached";
        return result;
    }

    // 2. Check rate limiter (atomic CAS — safe outside the reserve).
    if (!rate_limiter_.try_acquire())
    {
        PULSE_LOG_WARN("risk",
            "Order rejected: rate limit exceeded for {}", order.symbol);

        result.decision = RiskDecision::Rejected;
        result.approved_qty = 0.0;
        result.reason_code = ErrorCode::RateLimitHit;
        result.reason_message = "Order rate limit exceeded";
        return result;
    }

    // 3. Atomic check + reserve — single lock, no TOCTOU gaps.
    const auto reservation = position_manager_.reserve_notional(
        order.symbol, order.quantity, order.price);

    result.decision = reservation.decision;
    result.approved_qty = reservation.approved_qty;
    result.reason_code = reservation.reason_code;
    result.reason_message = reservation.reason_message;
    result.reservation_id = reservation.approved ? reservation.reservation_id : 0;

    return result;
}

// ---------------------------------------------------------------------------
// Futures order evaluation
// ---------------------------------------------------------------------------

RiskEvalResult RiskManager::evaluate_futures_order(
    const execution::OrderRequest &order, double leverage, double equity)
{
    RiskEvalResult result;

    // 1. Check leverage limit.
    if (leverage > config_.max_leverage)
    {
        PULSE_LOG_WARN("risk",
            "Futures order rejected: leverage {:.1f}x exceeds max {:.1f}x",
            leverage, config_.max_leverage);

        result.decision = RiskDecision::Rejected;
        result.approved_qty = 0.0;
        result.reason_code = ErrorCode::FuturesLeverageExceeded;
        result.reason_message = "Leverage exceeds maximum allowed";
        return result;
    }

    // 2. Check margin sufficiency.
    // proposed_margin = qty * price / leverage (simplified; quanto handled by caller)
    const double proposed_margin = order.quantity * order.price / leverage;
    const auto summary = position_manager_.portfolio_summary();
    const double total_margin_after = summary.total_margin_used + proposed_margin;
    const double max_margin = equity * config_.max_margin_used;

    if (total_margin_after > max_margin)
    {
        PULSE_LOG_WARN("risk",
            "Futures order rejected: margin {:.2f} + proposed {:.2f} exceeds {:.2f} "
            "({:.1f}% of equity {:.2f})",
            summary.total_margin_used, proposed_margin, max_margin,
            config_.max_margin_used * 100, equity);

        result.decision = RiskDecision::Rejected;
        result.approved_qty = 0.0;
        result.reason_code = ErrorCode::FuturesMarginInsufficient;
        result.reason_message = "Insufficient margin for futures position";
        return result;
    }

    // 3. Delegate to standard evaluate_order() for drawdown/rate/position limits.
    return evaluate_order(order);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

PositionManager &RiskManager::position_manager()
{
    return position_manager_;
}

DrawdownGuard &RiskManager::drawdown_guard()
{
    return drawdown_guard_;
}

OrderRateLimiter &RiskManager::rate_limiter()
{
    return rate_limiter_;
}

bool RiskManager::is_trading_halted() const
{
    return drawdown_guard_.is_halted();
}

RiskSnapshot RiskManager::risk_snapshot() const
{
    RiskSnapshot snap;
    snap.trading_halted = drawdown_guard_.is_halted();
    snap.halt_reason = drawdown_guard_.halt_reason();
    snap.daily_drawdown = drawdown_guard_.daily_drawdown();
    snap.max_drawdown = drawdown_guard_.max_drawdown();
    snap.rate_limiter_tokens = rate_limiter_.available_tokens();
    snap.rate_limiter_exhausted = rate_limiter_.is_exhausted();
    snap.portfolio = position_manager_.portfolio_summary();
    snap.open_position_count = position_manager_.open_position_count();
    return snap;
}

} // namespace pulse::risk
