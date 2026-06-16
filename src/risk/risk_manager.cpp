// risk_manager.cpp — Central order gate (Layer 7 Risk Management)

#include "pulse/risk/risk_manager.hpp"

#include "pulse/logging/logger.hpp"

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
    // 3. Check PositionManager: portfolio limits OK?
    //    - If full order fits: Approved
    //    - If partial order fits: Modified with reduced qty
    //    - If nothing fits: Rejected
    // 4. All checks passed: Approved with original qty

    RiskEvalResult result;

    // 1. Check drawdown guard.
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

    // 2. Check rate limiter.
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

    // 3. Check position limits.
    const double proposed_notional = order.quantity * order.price;

    if (position_manager_.can_open_position(order.symbol, order.quantity, order.price))
    {
        // Full order fits within all limits.
        result.decision = RiskDecision::Approved;
        result.approved_qty = order.quantity;
        result.reason_code = ErrorCode::Ok;
        result.reason_message = "";
        return result;
    }

    // Full order doesn't fit — check if a reduced order can fit.
    const auto summary = position_manager_.portfolio_summary();
    const double remaining_notional = config_.maxPositionNotional - summary.total_notional;
    const double sym_notional = position_manager_.symbol_notional(order.symbol);
    const double remaining_sym_notional = config_.maxSymbolNotional - sym_notional;

    // Use the smaller remaining budget.
    const double budget = std::min(remaining_notional, remaining_sym_notional);

    // Check position count limit.
    if (summary.open_position_count >= config_.maxOpenPositions)
    {
        PULSE_LOG_WARN("risk",
            "Order rejected: max open positions ({}) reached", config_.maxOpenPositions);

        result.decision = RiskDecision::Rejected;
        result.approved_qty = 0.0;
        result.reason_code = ErrorCode::PositionLimitHit;
        result.reason_message = "Maximum open positions reached";
        return result;
    }

    if (budget > 0.0 && order.price > 0.0)
    {
        // Compute maximum quantity that fits within remaining budget.
        const double reduced_qty = budget / order.price;

        if (reduced_qty > 0.0 && reduced_qty < order.quantity)
        {
            PULSE_LOG_INFO("risk",
                "Order modified: {} qty reduced from {} to {} (notional budget: {:.2f})",
                order.symbol, order.quantity, reduced_qty, budget);

            result.decision = RiskDecision::Modified;
            result.approved_qty = reduced_qty;
            result.reason_code = ErrorCode::Ok;
            result.reason_message = "Quantity reduced to fit position limit";
            return result;
        }
    }

    // Nothing fits — reject.
    PULSE_LOG_WARN("risk",
        "Order rejected: no remaining notional budget for {} (proposed: {:.2f})",
        order.symbol, proposed_notional);

    result.decision = RiskDecision::Rejected;
    result.approved_qty = 0.0;
    result.reason_code = ErrorCode::PositionLimitHit;
    result.reason_message = "Position notional limit reached";
    return result;
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

} // namespace pulse::risk
