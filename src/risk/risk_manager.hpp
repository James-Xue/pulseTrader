#pragma once
// risk_manager.hpp — Central order gate (Layer 7 Risk Management)
//
// Evaluates each proposed order against all active risk rules and returns
// one of three verdicts: approve, modify (reduced size), or reject (with
// reason code).
//
// RiskManager is stateless — it delegates to sub-components that each
// handle their own thread safety. The evaluate_order() flow:
//   1. DrawdownGuard: is trading halted? -> Rejected
//   2. OrderRateLimiter: token bucket has capacity? -> Rejected
//   3. PositionManager: portfolio limits OK? -> try reduce qty (Modified) or Rejected
//   4. All pass -> Approved
//
// Thread safety:
//   - RiskManager itself is stateless (delegates to sub-components)
//   - Each sub-component handles its own synchronization
//   - evaluate_order() can be called from multiple strategy threads concurrently

#include "core/config.hpp"
#include "execution/order_executor.hpp"
#include "risk/drawdown_guard.hpp"
#include "risk/order_rate_limiter.hpp"
#include "risk/position_manager.hpp"
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
    ///   2. position_manager — tracks open positions
    ///   3. drawdown_guard   — monitors drawdown
    ///   4. rate_limiter     — enforces rate limits
    RiskManager(const RiskConfig &config,
        PositionManager &position_manager,
        DrawdownGuard &drawdown_guard,
        OrderRateLimiter &rate_limiter);

    /// Evaluate a proposed order against all risk rules.
    ///
    /// Checks in order:
    ///   1. DrawdownGuard: is trading halted?
    ///   2. OrderRateLimiter: token bucket has capacity?
    ///   3. PositionManager: portfolio limits (max notional, max positions, per-symbol)?
    ///   4. If notional would exceed limit, attempt to reduce quantity (modify).
    ///
    /// Returns RiskEvalResult with decision, approved_qty, and reason.
    [[nodiscard]] RiskEvalResult evaluate_order(const execution::OrderRequest &order);

    // --- Access sub-components ---
    [[nodiscard]] PositionManager &position_manager();
    [[nodiscard]] DrawdownGuard &drawdown_guard();
    [[nodiscard]] OrderRateLimiter &rate_limiter();

    /// Quick check: is trading currently halted?
    [[nodiscard]] bool is_trading_halted() const;

    /// Returns an aggregated snapshot of all risk state.
    ///
    /// Bundles: halt status, drawdown levels, rate limiter state,
    /// and portfolio summary. Thread-safe: delegates to sub-components
    /// which each handle their own synchronization.
    [[nodiscard]] RiskSnapshot risk_snapshot() const;

  private:
    const RiskConfig &config_;
    PositionManager &position_manager_;
    DrawdownGuard &drawdown_guard_;
    OrderRateLimiter &rate_limiter_;
};

} // namespace pulse::risk
