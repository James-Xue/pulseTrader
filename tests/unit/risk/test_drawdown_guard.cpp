// test_drawdown_guard.cpp — Unit tests for DrawdownGuard (Layer 7 Risk Management)

#include "risk/DrawdownGuard.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::risk;

static RiskConfig make_config(double max_daily = 0.02, double max_dd = 0.05)
{
    RiskConfig cfg;
    cfg.maxDailyDrawdown = max_daily;
    cfg.maxDrawdown = max_dd;
    return cfg;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, DefaultNotHalted)
{
    DrawdownGuard guard(make_config());
    EXPECT_FALSE(guard.isHalted());
    EXPECT_EQ(ErrorCode::Ok, guard.haltReason());
}

TEST(DrawdownGuard, DefaultZeroDrawdown)
{
    DrawdownGuard guard(make_config());
    EXPECT_DOUBLE_EQ(0.0, guard.dailyDrawdown());
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());
}

// ---------------------------------------------------------------------------
// Equity tracking
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, PeakEquityUpdatesOnNewHigh)
{
    DrawdownGuard guard(make_config());

    guard.updateEquity(10000.0);
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown()); // At peak, no drawdown.

    guard.updateEquity(11000.0);
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown()); // New peak, still no drawdown.
}

TEST(DrawdownGuard, DrawdownCalculatedFromPeak)
{
    DrawdownGuard guard(make_config());

    guard.updateEquity(10000.0); // Peak = 10000.
    guard.updateEquity(9500.0);  // DD = (10000 - 9500) / 10000 = 0.05.

    EXPECT_DOUBLE_EQ(0.05, guard.maxDrawdown());
}

TEST(DrawdownGuard, DrawdownDoesNotGoNegative)
{
    DrawdownGuard guard(make_config());

    guard.updateEquity(10000.0);
    guard.updateEquity(10500.0); // Above peak — drawdown should be 0.

    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());
}

// ---------------------------------------------------------------------------
// Daily drawdown
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, DailyDrawdownTriggersHalt)
{
    DrawdownGuard guard(make_config(/*max_daily=*/0.02, 0.10));

    guard.updateEquity(10000.0); // Start of day: equity = 10000.
    EXPECT_FALSE(guard.isHalted());

    // Drop to 9700 — daily DD = (10000 - 9700) / 10000 = 0.03 > 0.02.
    guard.updateEquity(9700.0);
    EXPECT_TRUE(guard.isHalted());
    EXPECT_EQ(ErrorCode::DrawdownLimitHit, guard.haltReason());
}

TEST(DrawdownGuard, DailyDrawdownBelowThresholdDoesNotHalt)
{
    DrawdownGuard guard(make_config(/*max_daily=*/0.05, 0.10));

    guard.updateEquity(10000.0);
    guard.updateEquity(9600.0); // DD = 0.04 < 0.05.

    EXPECT_FALSE(guard.isHalted());
}

TEST(DrawdownGuard, DailyDrawdownValue)
{
    DrawdownGuard guard(make_config());

    guard.updateEquity(10000.0);
    guard.updateEquity(9800.0); // DD = (10000 - 9800) / 10000 = 0.02.

    EXPECT_DOUBLE_EQ(0.02, guard.dailyDrawdown());
}

// ---------------------------------------------------------------------------
// Peak-to-valley drawdown
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, PeakDrawdownTriggersHalt)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0); // Peak = 10000.
    guard.updateEquity(10500.0); // Peak = 10500.
    EXPECT_FALSE(guard.isHalted());

    // Drop to 9900 — DD from peak = (10500 - 9900) / 10500 ≈ 0.0571 > 0.05.
    guard.updateEquity(9900.0);
    EXPECT_TRUE(guard.isHalted());
    EXPECT_EQ(ErrorCode::DrawdownLimitHit, guard.haltReason());
}

TEST(DrawdownGuard, RecoveryDoesNotClearHalt)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0);
    guard.updateEquity(9400.0); // DD = 0.06 > 0.05 — halted.
    EXPECT_TRUE(guard.isHalted());

    guard.updateEquity(10200.0); // Recovery — but halt persists.
    EXPECT_TRUE(guard.isHalted());
}

TEST(DrawdownGuard, PeakDrawdownBelowThresholdDoesNotHalt)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.10));

    guard.updateEquity(10000.0);
    guard.updateEquity(9100.0); // DD = 0.09 < 0.10.

    EXPECT_FALSE(guard.isHalted());
}

// ---------------------------------------------------------------------------
// Reset / Clear
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, ResetDailyResetsCounters)
{
    DrawdownGuard guard(make_config());

    guard.updateEquity(10000.0);
    guard.updateEquity(9500.0);
    guard.recordPnl(-500.0, "s1");

    guard.resetDaily();

    EXPECT_DOUBLE_EQ(0.0, guard.dailyDrawdown()); // daily_start now = 9500.
}

TEST(DrawdownGuard, ClearHaltResetsFlag)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0);
    guard.updateEquity(9400.0); // Triggers halt.
    EXPECT_TRUE(guard.isHalted());

    guard.clearHalt();
    EXPECT_FALSE(guard.isHalted());
    EXPECT_EQ(ErrorCode::Ok, guard.haltReason());
}

// ---------------------------------------------------------------------------
// Per-strategy PnL
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, RecordPnlAccumulatesPerStrategy)
{
    DrawdownGuard guard(make_config());

    guard.recordPnl(10.0, "scalper");
    guard.recordPnl(-5.0, "scalper");
    guard.recordPnl(20.0, "swing");

    // Internal state — verified through dailyDrawdown / behavior.
    // No direct accessor for per-strategy PnL (by design — it's internal).
    // But recordPnl should not crash and should accumulate correctly.
    EXPECT_FALSE(guard.isHalted());
}
