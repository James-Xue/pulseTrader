// risk_manager.cpp — Central order gate (Layer 7 Risk Management)

#include "risk/RiskManager.hpp"

#include "logging/Logger.hpp"

#include <cmath>

namespace pulse::risk
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RiskManager::RiskManager(const RiskConfig &config,
    PositionManager &positionManager,
    DrawdownGuard &drawdownGuard,
    OrderRateLimiter &rateLimiter)
    : m_config{ config }
    , m_positionManager{ positionManager }
    , m_drawdownGuard{ drawdownGuard }
    , m_rateLimiter{ rateLimiter }
{
}

// ---------------------------------------------------------------------------
// Order evaluation
// ---------------------------------------------------------------------------

RiskEvalResult RiskManager::evaluateOrder(const execution::OrderRequest &order)
{
    // Evaluate a proposed order against all risk rules sequentially:
    // 1. Check DrawdownGuard: is trading halted?
    // 2. Check OrderRateLimiter: token bucket has capacity?
    // 3. Atomically check + reserve via PositionManager (TOCTOU-safe):
    //    - If full order fits: Approved
    //    - If partial order fits: Modified with reduced qty
    //    - If nothing fits: Rejected
    //
    // The atomic reserveNotional() call replaces the old 3-separate-lock
    // pattern (canOpenPosition + portfolioSummary + symbolNotional)
    // that was vulnerable to TOCTOU races under concurrent strategy threads.

    RiskEvalResult result;

    // 1. Check drawdown guard (atomic load — safe outside the reserve).
    if (m_drawdownGuard.isHalted())
    {
        PULSE_LOG_WARN("risk",
            "Order rejected: trading halted (reason: {})",
            static_cast<int>(m_drawdownGuard.haltReason()));

        result.decision = RiskDecision::Rejected;
        result.approved_qty = 0.0;
        result.reason_code = m_drawdownGuard.haltReason();
        result.reason_message = "Trading halted: drawdown limit breached";
        return result;
    }

    // 2. Check rate limiter (atomic CAS — safe outside the reserve).
    if (!m_rateLimiter.tryAcquire())
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
    const auto reservation = m_positionManager.reserveNotional(
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

RiskEvalResult RiskManager::evaluateFuturesOrder(
    const execution::OrderRequest &order, double leverage, double equity)
{
    RiskEvalResult result;

    // 1. Check leverage limit.
    if (leverage > m_config.max_leverage)
    {
        PULSE_LOG_WARN("risk",
            "Futures order rejected: leverage {:.1f}x exceeds max {:.1f}x",
            leverage, m_config.max_leverage);

        result.decision = RiskDecision::Rejected;
        result.approved_qty = 0.0;
        result.reason_code = ErrorCode::FuturesLeverageExceeded;
        result.reason_message = "Leverage exceeds maximum allowed";
        return result;
    }

    // 2. Check margin sufficiency.
    // proposed_margin = qty * price / leverage (simplified; quanto handled by caller)
    const double proposed_margin = order.quantity * order.price / leverage;
    const auto summary = m_positionManager.portfolioSummary();
    const double total_margin_after = summary.total_margin_used + proposed_margin;
    const double max_margin = equity * m_config.max_margin_used;

    if (total_margin_after > max_margin)
    {
        PULSE_LOG_WARN("risk",
            "Futures order rejected: margin {:.2f} + proposed {:.2f} exceeds {:.2f} "
            "({:.1f}% of equity {:.2f})",
            summary.total_margin_used, proposed_margin, max_margin,
            m_config.max_margin_used * 100, equity);

        result.decision = RiskDecision::Rejected;
        result.approved_qty = 0.0;
        result.reason_code = ErrorCode::FuturesMarginInsufficient;
        result.reason_message = "Insufficient margin for futures position";
        return result;
    }

    // 3. Delegate to standard evaluateOrder() for drawdown/rate/position limits.
    return evaluateOrder(order);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

PositionManager &RiskManager::positionManager()
{
    return m_positionManager;
}

DrawdownGuard &RiskManager::drawdownGuard()
{
    return m_drawdownGuard;
}

OrderRateLimiter &RiskManager::rateLimiter()
{
    return m_rateLimiter;
}

bool RiskManager::isTradingHalted() const
{
    return m_drawdownGuard.isHalted();
}

RiskSnapshot RiskManager::riskSnapshot() const
{
    RiskSnapshot snap;
    snap.trading_halted = m_drawdownGuard.isHalted();
    snap.haltReason = m_drawdownGuard.haltReason();
    snap.dailyDrawdown = m_drawdownGuard.dailyDrawdown();
    snap.maxDrawdown = m_drawdownGuard.maxDrawdown();
    snap.rate_limiter_tokens = m_rateLimiter.availableTokens();
    snap.rate_limiter_exhausted = m_rateLimiter.isExhausted();
    snap.portfolio = m_positionManager.portfolioSummary();
    snap.openPositionCount = m_positionManager.openPositionCount();
    return snap;
}

} // namespace pulse::risk
