// test_risk_manager.cpp — Unit tests for RiskManager (Layer 7 Risk Management)

#include "risk/risk_manager.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::risk;
using namespace pulse::execution;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class RiskManagerTest : public ::testing::Test
{
  protected:
    RiskConfig config_;
    PositionManager position_manager_{ config_ };
    DrawdownGuard drawdown_guard_{ config_ };
    OrderRateLimiter rate_limiter_{ config_.maxOrdersPerSec };
    RiskManager risk_manager_{ config_, position_manager_, drawdown_guard_, rate_limiter_ };

    static OrderRequest make_order(const std::string &symbol = "BTC_USDT",
        Side side = Side::Buy, Quantity qty = 0.001, Price price = 50000.0)
    {
        OrderRequest req;
        req.symbol = symbol;
        req.side = side;
        req.type = OrderType::Limit;
        req.quantity = qty;
        req.price = price;
        return req;
    }
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_F(RiskManagerTest, DefaultNotHalted)
{
    EXPECT_FALSE(risk_manager_.is_trading_halted());
}

// ---------------------------------------------------------------------------
// Approval
// ---------------------------------------------------------------------------

TEST_F(RiskManagerTest, OrderWithinAllLimitsApproved)
{
    const auto result = risk_manager_.evaluate_order(make_order());
    EXPECT_EQ(RiskDecision::Approved, result.decision);
    EXPECT_DOUBLE_EQ(0.001, result.approved_qty);
    EXPECT_EQ(ErrorCode::Ok, result.reason_code);
}

TEST_F(RiskManagerTest, MultipleOrdersApproved)
{
    const auto r1 = risk_manager_.evaluate_order(make_order("BTC_USDT", Side::Buy, 0.001, 50000.0));
    EXPECT_EQ(RiskDecision::Approved, r1.decision);

    const auto r2 = risk_manager_.evaluate_order(make_order("ETH_USDT", Side::Buy, 0.1, 3000.0));
    EXPECT_EQ(RiskDecision::Approved, r2.decision);
}

// ---------------------------------------------------------------------------
// Drawdown rejection
// ---------------------------------------------------------------------------

TEST_F(RiskManagerTest, HaltedDrawdownRejectsOrder)
{
    // Trigger halt: equity drops more than 5% from peak.
    drawdown_guard_.update_equity(10000.0);
    drawdown_guard_.update_equity(9400.0); // 6% drawdown > 5% threshold.

    const auto result = risk_manager_.evaluate_order(make_order());
    EXPECT_EQ(RiskDecision::Rejected, result.decision);
    EXPECT_DOUBLE_EQ(0.0, result.approved_qty);
    EXPECT_EQ(ErrorCode::DrawdownLimitHit, result.reason_code);
}

TEST_F(RiskManagerTest, HaltedFlagReflectedInIsTradingHalted)
{
    drawdown_guard_.update_equity(10000.0);
    drawdown_guard_.update_equity(9400.0);

    EXPECT_TRUE(risk_manager_.is_trading_halted());
}

// ---------------------------------------------------------------------------
// Rate limit rejection
// ---------------------------------------------------------------------------

TEST_F(RiskManagerTest, ExhaustedRateLimiterRejectsOrder)
{
    // Exhaust all rate limiter tokens (default: 5 rate, burst = 10).
    for (int i = 0; i < 10; ++i)
    {
        (void)risk_manager_.evaluate_order(make_order());
    }

    const auto result = risk_manager_.evaluate_order(make_order());
    EXPECT_EQ(RiskDecision::Rejected, result.decision);
    EXPECT_EQ(ErrorCode::RateLimitHit, result.reason_code);
}

TEST_F(RiskManagerTest, RateLimiterDoesNotConsumeTokenOnDrawdownReject)
{
    // Halt trading first.
    drawdown_guard_.update_equity(10000.0);
    drawdown_guard_.update_equity(9400.0);

    // These should all fail on drawdown, not consuming rate tokens.
    for (int i = 0; i < 20; ++i)
    {
        (void)risk_manager_.evaluate_order(make_order());
    }

    // Clear halt.
    drawdown_guard_.clear_halt();

    // Rate limiter should still have capacity.
    const auto result = risk_manager_.evaluate_order(make_order());
    EXPECT_EQ(RiskDecision::Approved, result.decision);
}

// ---------------------------------------------------------------------------
// Position limit rejection
// ---------------------------------------------------------------------------

TEST_F(RiskManagerTest, MaxPositionsReachedRejectsOrder)
{
    // Default max positions = 5.
    for (int i = 0; i < 5; ++i)
    {
        auto r = risk_manager_.evaluate_order(make_order(
            "SYM_" + std::to_string(i), Side::Buy, 0.001, 10.0));
        EXPECT_EQ(RiskDecision::Approved, r.decision);

        // Actually open the position so it counts.
        (void)position_manager_.open_position(
            "SYM_" + std::to_string(i), Side::Buy, 0.001, 10.0, "s1");
    }

    // 6th order should be rejected.
    const auto result = risk_manager_.evaluate_order(make_order("NEW_SYM", Side::Buy, 0.001, 10.0));
    EXPECT_EQ(RiskDecision::Rejected, result.decision);
    EXPECT_EQ(ErrorCode::PositionLimitHit, result.reason_code);
}

TEST_F(RiskManagerTest, MaxNotionalReachedRejectsOrder)
{
    // Default maxPositionNotional = 1000, maxSymbolNotional = 500 USDT.
    // Fill budget: 450 + 450 + 100 = 1000 USDT across 3 symbols.
    (void)position_manager_.open_position("BTC_USDT", Side::Buy, 0.009, 50000.0, "s1"); // 450
    (void)position_manager_.open_position("ETH_USDT", Side::Buy, 0.15, 3000.0, "s1");   // 450
    (void)position_manager_.open_position("SOL_USDT", Side::Buy, 1.0, 100.0, "s1");     // 100

    // Any new order should be rejected (no remaining budget).
    const auto result = risk_manager_.evaluate_order(make_order("DOGE_USDT", Side::Buy, 100.0, 0.5));
    EXPECT_EQ(RiskDecision::Rejected, result.decision);
    EXPECT_EQ(ErrorCode::PositionLimitHit, result.reason_code);
}

TEST_F(RiskManagerTest, SymbolNotionalReachedRejectsOrder)
{
    // Default maxSymbolNotional = 500 USDT.
    // Open a 450 USDT BTC position.
    (void)position_manager_.open_position("BTC_USDT", Side::Buy, 0.009, 50000.0, "s1");

    // Another BTC order for 0.005 * 50000 = 250 USDT.
    // Remaining symbol budget = 50 USDT -> 50/50000 = 0.001 BTC.
    // Since 0.001 < 0.005, it should be Modified (not rejected).
    const auto result = risk_manager_.evaluate_order(make_order("BTC_USDT", Side::Buy, 0.005, 50000.0));
    EXPECT_EQ(RiskDecision::Modified, result.decision);
    EXPECT_NEAR(0.001, result.approved_qty, 0.0001);
}

// ---------------------------------------------------------------------------
// Modification (quantity reduced)
// ---------------------------------------------------------------------------

TEST_F(RiskManagerTest, QuantityReducedWhenPartialNotionalAvailable)
{
    // Default maxPositionNotional = 1000, maxSymbolNotional = 500 USDT.
    // Open positions using up 800 USDT total, within per-symbol limits.
    (void)position_manager_.open_position("ETH_USDT", Side::Buy, 0.1, 3000.0, "s1");  // 300
    (void)position_manager_.open_position("SOL_USDT", Side::Buy, 5.0, 100.0, "s1");   // 500

    // Remaining total budget = 1000 - 800 = 200 USDT.
    // Remaining SOL symbol budget = 500 - 500 = 0. Use a new symbol.
    // Propose BTC order: 0.01 * 50000 = 500 USDT -> reduced to 200/50000 = 0.004.
    const auto result = risk_manager_.evaluate_order(make_order("BTC_USDT", Side::Buy, 0.01, 50000.0));
    EXPECT_EQ(RiskDecision::Modified, result.decision);
    EXPECT_NEAR(0.004, result.approved_qty, 0.0001);
}

TEST_F(RiskManagerTest, QuantityReducedBySymbolLimit)
{
    // Default maxSymbolNotional = 500 USDT.
    // Open a 350 USDT BTC position.
    (void)position_manager_.open_position("BTC_USDT", Side::Buy, 0.007, 50000.0, "s1");

    // Remaining symbol budget = 150 USDT.
    // Propose: 0.01 * 50000 = 500 USDT -> reduced to 150/50000 = 0.003.
    const auto result = risk_manager_.evaluate_order(make_order("BTC_USDT", Side::Buy, 0.01, 50000.0));
    EXPECT_EQ(RiskDecision::Modified, result.decision);
    EXPECT_DOUBLE_EQ(0.003, result.approved_qty);
}

// ---------------------------------------------------------------------------
// Integration: full flow
// ---------------------------------------------------------------------------

TEST_F(RiskManagerTest, FullFlowApproval)
{
    // Ensure all subsystems are healthy.
    drawdown_guard_.update_equity(10000.0);

    const auto result = risk_manager_.evaluate_order(make_order("BTC_USDT", Side::Buy, 0.001, 50000.0));
    EXPECT_EQ(RiskDecision::Approved, result.decision);
    EXPECT_DOUBLE_EQ(0.001, result.approved_qty);
}

TEST_F(RiskManagerTest, SubComponentsAccessible)
{
    // Verify sub-components can be accessed.
    EXPECT_EQ(0, risk_manager_.position_manager().open_position_count());
    EXPECT_FALSE(risk_manager_.drawdown_guard().is_halted());
    EXPECT_FALSE(risk_manager_.rate_limiter().is_exhausted());
}

TEST_F(RiskManagerTest, ClearHaltAllowsTradingAgain)
{
    // Halt, verify rejection.
    drawdown_guard_.update_equity(10000.0);
    drawdown_guard_.update_equity(9400.0);
    EXPECT_EQ(RiskDecision::Rejected, risk_manager_.evaluate_order(make_order()).decision);

    // Clear halt, verify approval.
    drawdown_guard_.clear_halt();
    EXPECT_EQ(RiskDecision::Approved, risk_manager_.evaluate_order(make_order()).decision);
}

// ---------------------------------------------------------------------------
// risk_snapshot() — interface gap bridge for dashboard
// ---------------------------------------------------------------------------

TEST_F(RiskManagerTest, RiskSnapshotDefaultAllZerosAndFalse)
{
    // A fresh risk manager must return a snapshot with all default values.
    const auto snap = risk_manager_.risk_snapshot();

    EXPECT_FALSE(snap.trading_halted);
    EXPECT_EQ(ErrorCode::Ok, snap.halt_reason);
    EXPECT_DOUBLE_EQ(0.0, snap.daily_drawdown);
    EXPECT_DOUBLE_EQ(0.0, snap.max_drawdown);
    EXPECT_FALSE(snap.rate_limiter_exhausted);
    EXPECT_EQ(0, snap.portfolio.open_position_count);
    EXPECT_DOUBLE_EQ(0.0, snap.portfolio.total_notional);
    EXPECT_DOUBLE_EQ(0.0, snap.portfolio.total_unrealized_pnl);
    EXPECT_DOUBLE_EQ(0.0, snap.portfolio.net_exposure);
    EXPECT_EQ(0, snap.open_position_count);
}

TEST_F(RiskManagerTest, RiskSnapshotReflectsOpenPositions)
{
    // Open positions and verify the snapshot reflects them.
    (void)position_manager_.open_position("BTC_USDT", Side::Buy, 0.001, 50000.0, "s1");
    (void)position_manager_.open_position("ETH_USDT", Side::Buy, 0.1, 3000.0, "s1");

    const auto snap = risk_manager_.risk_snapshot();

    EXPECT_EQ(2, snap.open_position_count);
    EXPECT_EQ(2, snap.portfolio.open_position_count);
    // total_notional = 0.001 * 50000 + 0.1 * 3000 = 50 + 300 = 350
    EXPECT_DOUBLE_EQ(350.0, snap.portfolio.total_notional);
    // net_exposure for two longs = 350
    EXPECT_DOUBLE_EQ(350.0, snap.portfolio.net_exposure);
}

TEST_F(RiskManagerTest, RiskSnapshotReflectsHaltedState)
{
    // Trigger a drawdown halt and verify the snapshot reflects it.
    drawdown_guard_.update_equity(10000.0);
    drawdown_guard_.update_equity(9400.0); // 6% drawdown > 5% threshold.

    const auto snap = risk_manager_.risk_snapshot();

    EXPECT_TRUE(snap.trading_halted);
    EXPECT_NE(ErrorCode::Ok, snap.halt_reason);
    EXPECT_GT(snap.max_drawdown, 0.0);
}

// ---------------------------------------------------------------------------
// M11: Futures order evaluation tests
// ---------------------------------------------------------------------------

TEST_F(RiskManagerTest, FuturesOrder_LeverageExceeded)
{
    // config_.max_leverage defaults to 10.0
    // Requesting 20x leverage should be rejected with FuturesLeverageExceeded (7001).
    const auto order = make_order();
    const auto result = risk_manager_.evaluate_futures_order(order, 20.0, 10000.0);

    EXPECT_EQ(RiskDecision::Rejected, result.decision);
    EXPECT_EQ(ErrorCode::FuturesLeverageExceeded, result.reason_code);
}

TEST_F(RiskManagerTest, FuturesOrder_MarginInsufficient)
{
    // config_.max_margin_used defaults to 0.5 (50% of equity)
    // equity = 1000, max margin = 500
    // proposed margin = 0.01 * 50000 / 5 = 100 → OK (within 500)
    // But let's make it exceed: qty=1, price=50000, leverage=2 → margin=25000 > 500
    OrderRequest order;
    order.symbol = "BTC_USDT";
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.quantity = 1.0;
    order.price = 50000.0;

    const auto result = risk_manager_.evaluate_futures_order(order, 2.0, 1000.0);

    EXPECT_EQ(RiskDecision::Rejected, result.decision);
    EXPECT_EQ(ErrorCode::FuturesMarginInsufficient, result.reason_code);
}

TEST_F(RiskManagerTest, FuturesOrder_Approved)
{
    // Reasonable futures order within all limits.
    // equity=10000, leverage=5x, qty=0.001, price=50000
    // margin = 0.001 * 50000 / 5 = 10 → well within 5000 (50% of 10000)
    const auto order = make_order("BTC_USDT", Side::Buy, 0.001, 50000.0);
    const auto result = risk_manager_.evaluate_futures_order(order, 5.0, 10000.0);

    EXPECT_EQ(RiskDecision::Approved, result.decision);
    EXPECT_DOUBLE_EQ(0.001, result.approved_qty);
    EXPECT_EQ(ErrorCode::Ok, result.reason_code);
}

TEST_F(RiskManagerTest, FuturesOrder_LeverageAtBoundary)
{
    // Leverage exactly at max_leverage should be approved.
    config_.max_leverage = 10.0;
    const auto order = make_order("BTC_USDT", Side::Buy, 0.001, 50000.0);
    const auto result = risk_manager_.evaluate_futures_order(order, 10.0, 10000.0);

    EXPECT_NE(RiskDecision::Rejected, result.decision);
}
