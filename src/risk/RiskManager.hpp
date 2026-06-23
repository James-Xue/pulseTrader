#pragma once
// risk_manager.hpp — Central order gate (Layer 7 Risk Management)
//
// Evaluates each proposed order against all active risk rules and returns
// one of three verdicts: approve, modify (reduced size), or reject (with
// reason code).
//
// RiskManager is stateless — it delegates to sub-components that each
// handle their own thread safety. The evaluateOrder() flow:
//   1. DrawdownGuard: is trading halted? -> Rejected
//   2. OrderRateLimiter: token bucket has capacity? -> Rejected
//   3. PositionManager: portfolio limits OK? -> try reduce qty (Modified) or Rejected
//   4. All pass -> Approved
//
// Thread safety:
//   - RiskManager itself is stateless (delegates to sub-components)
//   - Each sub-component handles its own synchronization
//   - evaluateOrder() can be called from multiple strategy threads concurrently

#include "core/config.hpp"
#include "execution/OrderExecutor.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/OrderRateLimiter.hpp"
#include "risk/PositionManager.hpp"
#include "risk/risk_types.hpp"

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// RiskManager — central order gate composing all risk checks
// ---------------------------------------------------------------------------
class RiskManager
{
  public:
    /// Construct with references to all risk sub-components.
    ///
    /// Parameters:
    ///   1. config           — RiskConfig with limits
    ///   2. positionManager — tracks open positions
    ///   3. drawdownGuard   — monitors drawdown
    ///   4. rateLimiter     — enforces rate limits
    RiskManager(const RiskConfig &config,
        PositionManager &positionManager,
        DrawdownGuard &drawdownGuard,
        OrderRateLimiter &rateLimiter);

    /// Evaluate a proposed order against all risk rules.
    ///
    /// Checks in order:
    ///   1. DrawdownGuard: is trading halted?
    ///   2. OrderRateLimiter: token bucket has capacity?
    ///   3. PositionManager: portfolio limits (max notional, max positions, per-symbol)?
    ///   4. If notional would exceed limit, attempt to reduce quantity (modify).
    ///
    /// Returns RiskEvalResult with decision, approved_qty, and reason.
    [[nodiscard]] RiskEvalResult evaluateOrder(const execution::OrderRequest &order);

    /// Evaluate a futures order — adds leverage and margin checks before
    /// delegating to evaluateOrder() for the standard drawdown/rate/position checks.
    ///
    /// Additional checks:
    ///   1. Leverage <= config.max_leverage → else FuturesLeverageExceeded (7001)
    ///   2. Total margin + proposed margin <= equity * config.max_margin_used → else FuturesMarginInsufficient (7002)
    ///
    /// Parameters:
    ///   - order:    the proposed order
    ///   - leverage: requested leverage multiplier
    ///   - equity:   current account equity (for margin sufficiency check)
    [[nodiscard]] RiskEvalResult evaluateFuturesOrder(
        const execution::OrderRequest &order, double leverage, double equity);

    // --- Access sub-components ---
    [[nodiscard]] PositionManager &positionManager();
    [[nodiscard]] DrawdownGuard &drawdownGuard();
    [[nodiscard]] OrderRateLimiter &rateLimiter();

    /// Quick check: is trading currently halted?
    [[nodiscard]] bool isTradingHalted() const;

    /// Returns an aggregated snapshot of all risk state.
    ///
    /// Bundles: halt status, drawdown levels, rate limiter state,
    /// and portfolio summary. Thread-safe: delegates to sub-components
    /// which each handle their own synchronization.
    [[nodiscard]] RiskSnapshot riskSnapshot() const;

  private:
    const RiskConfig &m_config;
    PositionManager &m_positionManager;
    DrawdownGuard &m_drawdownGuard;
    OrderRateLimiter &m_rateLimiter;
};

} // namespace pulse::risk
