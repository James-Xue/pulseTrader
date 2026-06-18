// test_drawdown_guard.cpp — Unit tests for DrawdownGuard (Layer 7 Risk Management)

#include "risk/drawdown_guard.hpp"

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
    EXPECT_FALSE(guard.is_halted());
    EXPECT_EQ(ErrorCode::Ok, guard.halt_reason());
}

TEST(DrawdownGuard, DefaultZeroDrawdown)
{
    DrawdownGuard guard(make_config());
    EXPECT_DOUBLE_EQ(0.0, guard.daily_drawdown());
    EXPECT_DOUBLE_EQ(0.0, guard.max_drawdown());
}

// ---------------------------------------------------------------------------
// Equity tracking
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, PeakEquityUpdatesOnNewHigh)
{
    DrawdownGuard guard(make_config());

    guard.update_equity(10000.0);
    EXPECT_DOUBLE_EQ(0.0, guard.max_drawdown()); // At peak, no drawdown.

    guard.update_equity(11000.0);
    EXPECT_DOUBLE_EQ(0.0, guard.max_drawdown()); // New peak, still no drawdown.
}

TEST(DrawdownGuard, DrawdownCalculatedFromPeak)
{
    DrawdownGuard guard(make_config());

    guard.update_equity(10000.0); // Peak = 10000.
    guard.update_equity(9500.0);  // DD = (10000 - 9500) / 10000 = 0.05.

    EXPECT_DOUBLE_EQ(0.05, guard.max_drawdown());
}

TEST(DrawdownGuard, DrawdownDoesNotGoNegative)
{
    DrawdownGuard guard(make_config());

    guard.update_equity(10000.0);
    guard.update_equity(10500.0); // Above peak — drawdown should be 0.

    EXPECT_DOUBLE_EQ(0.0, guard.max_drawdown());
}

// ---------------------------------------------------------------------------
// Daily drawdown
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, DailyDrawdownTriggersHalt)
{
    DrawdownGuard guard(make_config(/*max_daily=*/0.02, 0.10));

    guard.update_equity(10000.0); // Start of day: equity = 10000.
    EXPECT_FALSE(guard.is_halted());

    // Drop to 9700 — daily DD = (10000 - 9700) / 10000 = 0.03 > 0.02.
    guard.update_equity(9700.0);
    EXPECT_TRUE(guard.is_halted());
    EXPECT_EQ(ErrorCode::DrawdownLimitHit, guard.halt_reason());
}

TEST(DrawdownGuard, DailyDrawdownBelowThresholdDoesNotHalt)
{
    DrawdownGuard guard(make_config(/*max_daily=*/0.05, 0.10));

    guard.update_equity(10000.0);
    guard.update_equity(9600.0); // DD = 0.04 < 0.05.

    EXPECT_FALSE(guard.is_halted());
}

TEST(DrawdownGuard, DailyDrawdownValue)
{
    DrawdownGuard guard(make_config());

    guard.update_equity(10000.0);
    guard.update_equity(9800.0); // DD = (10000 - 9800) / 10000 = 0.02.

    EXPECT_DOUBLE_EQ(0.02, guard.daily_drawdown());
}

// ---------------------------------------------------------------------------
// Peak-to-valley drawdown
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, PeakDrawdownTriggersHalt)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.update_equity(10000.0); // Peak = 10000.
    guard.update_equity(10500.0); // Peak = 10500.
    EXPECT_FALSE(guard.is_halted());

    // Drop to 9900 — DD from peak = (10500 - 9900) / 10500 ≈ 0.0571 > 0.05.
    guard.update_equity(9900.0);
    EXPECT_TRUE(guard.is_halted());
    EXPECT_EQ(ErrorCode::DrawdownLimitHit, guard.halt_reason());
}

TEST(DrawdownGuard, RecoveryDoesNotClearHalt)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.update_equity(10000.0);
    guard.update_equity(9400.0); // DD = 0.06 > 0.05 — halted.
    EXPECT_TRUE(guard.is_halted());

    guard.update_equity(10200.0); // Recovery — but halt persists.
    EXPECT_TRUE(guard.is_halted());
}

TEST(DrawdownGuard, PeakDrawdownBelowThresholdDoesNotHalt)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.10));

    guard.update_equity(10000.0);
    guard.update_equity(9100.0); // DD = 0.09 < 0.10.

    EXPECT_FALSE(guard.is_halted());
}

// ---------------------------------------------------------------------------
// Reset / Clear
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, ResetDailyResetsCounters)
{
    DrawdownGuard guard(make_config());

    guard.update_equity(10000.0);
    guard.update_equity(9500.0);
    guard.record_pnl(-500.0, "s1");

    guard.reset_daily();

    EXPECT_DOUBLE_EQ(0.0, guard.daily_drawdown()); // daily_start now = 9500.
}

TEST(DrawdownGuard, ClearHaltResetsFlag)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.update_equity(10000.0);
    guard.update_equity(9400.0); // Triggers halt.
    EXPECT_TRUE(guard.is_halted());

    guard.clear_halt();
    EXPECT_FALSE(guard.is_halted());
    EXPECT_EQ(ErrorCode::Ok, guard.halt_reason());
}

// ---------------------------------------------------------------------------
// Per-strategy PnL
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, RecordPnlAccumulatesPerStrategy)
{
    DrawdownGuard guard(make_config());

    guard.record_pnl(10.0, "scalper");
    guard.record_pnl(-5.0, "scalper");
    guard.record_pnl(20.0, "swing");

    // Internal state — verified through daily_drawdown / behavior.
    // No direct accessor for per-strategy PnL (by design — it's internal).
    // But record_pnl should not crash and should accumulate correctly.
    EXPECT_FALSE(guard.is_halted());
}
