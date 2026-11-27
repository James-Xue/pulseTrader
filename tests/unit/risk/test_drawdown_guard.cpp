// test_drawdown_guard.cpp — Unit tests for DrawdownGuard (Layer 7 Risk Management)

#include "risk/DrawdownGuard.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>

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

TEST(DrawdownGuard, SeedRestoresPeakAndDailyBaselines)
{
    // Paper mode persists both baselines; a restart must not reset them to
    // the first post-restart snapshot.
    DrawdownGuard guard(make_config(0.20, 0.40));
    guard.seedEquity(10500.0, 10000.0);

    const auto day = std::chrono::duration_cast<std::chrono::hours>(
        std::chrono::system_clock::now().time_since_epoch()).count() / 24;
    guard.updateEquity(9500.0, day);

    EXPECT_NEAR(0.05, guard.dailyDrawdown(), 1e-9);
    EXPECT_NEAR((10500.0 - 9500.0) / 10500.0, guard.maxDrawdown(), 1e-9);
    EXPECT_FALSE(guard.isHalted());
}

TEST(DrawdownGuard, SeedIgnoresInvalidValues)
{
    DrawdownGuard guard(make_config());
    guard.seedEquity(0.0, 10000.0);
    guard.seedEquity(10000.0, -1.0);

    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());
    EXPECT_FALSE(guard.isHalted());
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

// ---------------------------------------------------------------------------
// Manual halt (CLI/MCP circuit-breaker override)
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, ManualHaltSetsHaltState)
{
    DrawdownGuard guard(make_config());
    guard.manualHalt();
    EXPECT_TRUE(guard.isHalted());
    EXPECT_EQ(ErrorCode::ManualHalt, guard.haltReason());
}

TEST(DrawdownGuard, ManualHaltWithCustomReason)
{
    DrawdownGuard guard(make_config());
    guard.manualHalt(ErrorCode::StopLossTriggered);
    EXPECT_TRUE(guard.isHalted());
    EXPECT_EQ(ErrorCode::StopLossTriggered, guard.haltReason());
}

TEST(DrawdownGuard, ClearHaltResumesAfterManualHalt)
{
    DrawdownGuard guard(make_config());
    guard.manualHalt();
    guard.clearHalt();
    EXPECT_FALSE(guard.isHalted());
    EXPECT_EQ(ErrorCode::Ok, guard.haltReason());
}

TEST(DrawdownGuard, RecordPnlDoesNotClearManualHalt)
{
    DrawdownGuard guard(make_config());
    guard.updateEquity(1000.0);
    guard.manualHalt();
    guard.recordPnl(50.0);
    EXPECT_TRUE(guard.isHalted());
    EXPECT_EQ(ErrorCode::ManualHalt, guard.haltReason());
}

TEST(DrawdownGuard, ManualHaltOverridesAfterClear)
{
    DrawdownGuard guard(make_config());
    guard.updateEquity(1000.0);
    // Trigger an automatic drawdown halt first.
    guard.updateEquity(900.0); // 10% peak-to-valley > 5% threshold
    EXPECT_TRUE(guard.isHalted());
    guard.clearHalt();
    EXPECT_FALSE(guard.isHalted());

    // Manual halt works after clearing.
    guard.manualHalt();
    EXPECT_TRUE(guard.isHalted());
    EXPECT_EQ(ErrorCode::ManualHalt, guard.haltReason());
}

// ---------------------------------------------------------------------------
// UTC day roll (audit B2, 2026-11-13): the daily baseline must reset at the
// UTC day boundary, otherwise the "daily" limit becomes "since process start".
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, NewUtcDayRebasesDailyBaseline)
{
    DrawdownGuard guard(make_config());

    // Day 20000: a 3% drop breaches the 2% daily limit.
    guard.updateEquity(10000.0, 20000);
    guard.updateEquity(9700.0, 20000);
    EXPECT_TRUE(guard.isHalted());
    EXPECT_NEAR(0.03, guard.dailyDrawdown(), 1e-9);

    // Operator clears the halt; the next UTC day starts from the new equity.
    guard.clearHalt();
    guard.updateEquity(9700.0, 20001);
    EXPECT_DOUBLE_EQ(0.0, guard.dailyDrawdown());
    EXPECT_FALSE(guard.isHalted());

    // A further 1% drop on the new day is below the 2% daily limit.
    // Peak-to-valley is 3.97%, still below the 5% limit.
    guard.updateEquity(9603.0, 20001);
    EXPECT_NEAR(0.01, guard.dailyDrawdown(), 1e-9);
    EXPECT_FALSE(guard.isHalted());
}

TEST(DrawdownGuard, SameUtcDayDoesNotRebase)
{
    DrawdownGuard guard(make_config());

    guard.updateEquity(10000.0, 20000);
    guard.updateEquity(9900.0, 20000);
    EXPECT_NEAR(0.01, guard.dailyDrawdown(), 1e-9);

    // Another snapshot on the SAME day must keep the original baseline.
    guard.updateEquity(9850.0, 20000);
    EXPECT_NEAR(0.015, guard.dailyDrawdown(), 1e-9);
    EXPECT_FALSE(guard.isHalted());
}

// ---------------------------------------------------------------------------
// Drawdown equity aggregation (2026-11-17: spot cash must not feed the breaker)
// ---------------------------------------------------------------------------

TEST(DrawdownEquity, SumsEveryMarkToMarketLeg)
{
    DrawdownEquityInputs in;
    in.futures_total = 19.45;
    in.cfd_equity = 100.0;

    const auto out = computeDrawdownEquity(in);

    EXPECT_TRUE(out.complete);
    EXPECT_DOUBLE_EQ(119.45, out.total);
}

TEST(DrawdownEquity, ZeroBalanceIsAValidLegNotAMissingOne)
{
    DrawdownEquityInputs in;
    in.futures_total = 0.0;
    in.cfd_equity = 0.0;

    const auto out = computeDrawdownEquity(in);

    EXPECT_TRUE(out.complete);
    EXPECT_DOUBLE_EQ(0.0, out.total);
}

TEST(DrawdownEquity, MissingLegMakesTheSnapshotIncomplete)
{
    DrawdownEquityInputs missing_futures;
    missing_futures.cfd_equity = 100.0;
    const auto a = computeDrawdownEquity(missing_futures);
    EXPECT_FALSE(a.complete);
    EXPECT_DOUBLE_EQ(100.0, a.total);

    DrawdownEquityInputs missing_cfd;
    missing_cfd.futures_total = 19.45;
    const auto b = computeDrawdownEquity(missing_cfd);
    EXPECT_FALSE(b.complete);
    EXPECT_DOUBLE_EQ(19.45, b.total);

    const auto c = computeDrawdownEquity(DrawdownEquityInputs{});
    EXPECT_FALSE(c.complete);
    EXPECT_DOUBLE_EQ(0.0, c.total);
}

// ---------------------------------------------------------------------------
// Snapshot plausibility gate (2026-11-17: a bad 519.45 read against a 47.16
// account latched a 90.9% drawdown halt that no later snapshot could clear)
// ---------------------------------------------------------------------------

TEST(DrawdownGuard, ImplausibleUpwardJumpDoesNotArmThePeak)
{
    DrawdownGuard guard(make_config());

    guard.updateEquity(47.16);
    EXPECT_FALSE(guard.isHalted());

    // A single snapshot 11x the real account (the observed spot-cash spike).
    guard.updateEquity(519.45);

    // Dropped, not clamped: no peak, no halt, no drawdown.
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());
    EXPECT_FALSE(guard.isHalted());

    // The account returns to its real level — must still read as flat.
    guard.updateEquity(47.16);
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());
    EXPECT_FALSE(guard.isHalted());
}

TEST(DrawdownGuard, RepeatedElevatedLevelIsAcceptedAsCashFlow)
{
    DrawdownGuard guard(make_config(/*max_daily=*/0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0);

    // First sighting: 20000 is 100% above the peak → held back.
    guard.updateEquity(20000.0);
    EXPECT_FALSE(guard.isHalted());

    // Second sighting within the 2% confirmation band → a real deposit.
    guard.updateEquity(20100.0);
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());

    // Both baselines rebased to the deposit level: a 10% fall from 20100 is
    // measured against 20100, not against the pre-deposit 10000.
    guard.updateEquity(18090.0);
    EXPECT_NEAR(0.10, guard.maxDrawdown(), 1e-6);
    EXPECT_NEAR(0.10, guard.dailyDrawdown(), 1e-6);
}

TEST(DrawdownGuard, LargeDropGetsOneGraceSnapshot)
{
    // A >50% step between two heartbeats is far likelier a cash transfer out or
    // a bad read than a real mark-to-market loss, so it gets exactly ONE grace
    // snapshot. A level that persists is accepted on the next snapshot and
    // halts normally — a genuine crash is delayed by one heartbeat, never
    // forgiven.
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0);
    guard.updateEquity(4000.0); // -60% — first sighting is held back.
    EXPECT_FALSE(guard.isHalted());
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown()); // Snapshot dropped entirely.

    guard.updateEquity(4000.0); // Second sighting confirms it.
    EXPECT_TRUE(guard.isHalted());
    EXPECT_EQ(ErrorCode::DrawdownLimitHit, guard.haltReason());
    EXPECT_NEAR(0.60, guard.maxDrawdown(), 1e-9);
}

TEST(DrawdownGuard, TransferOutThatRevertsDoesNotHalt)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0);
    guard.updateEquity(4000.0); // Held back as an unconfirmed drop.
    guard.updateEquity(10000.0); // Cash came back — pending run cleared.
    EXPECT_FALSE(guard.isHalted());
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());

    // A later large drop must still start a FRESH grace run, not confirm the
    // stale one.
    guard.updateEquity(4000.0);
    EXPECT_FALSE(guard.isHalted());
}

TEST(DrawdownGuard, ModerateDropIsEvaluatedImmediately)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0);
    guard.updateEquity(6000.0); // -40% is inside the step limit — no grace.

    EXPECT_TRUE(guard.isHalted());
    EXPECT_NEAR(0.40, guard.maxDrawdown(), 1e-9);
}

TEST(DrawdownGuard, GrowthWithinTheStepLimitRaisesThePeak)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0);
    guard.updateEquity(14000.0); // +40% < 50% step limit — ordinary growth.
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());

    guard.updateEquity(13000.0); // -7.14% from the accepted peak.
    EXPECT_NEAR(1.0 - (13000.0 / 14000.0), guard.maxDrawdown(), 1e-9);
    EXPECT_TRUE(guard.isHalted());
}

TEST(DrawdownGuard, PendingHighResetsAfterEquityFallsBack)
{
    // If the pending run survived a normal snapshot, two separate bad reads
    // separated by a healthy one would confirm each other and re-arm the peak —
    // exactly the 2026-11-17 failure, one snapshot later.
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0);
    guard.updateEquity(20000.0); // Held back (1/2).
    guard.updateEquity(10000.0); // Healthy read — must clear the pending run.
    guard.updateEquity(20000.0); // A fresh sighting again, still held (1/2).

    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());
    EXPECT_FALSE(guard.isHalted());

    guard.updateEquity(10000.0); // Must not read as a 50% drawdown.
    EXPECT_DOUBLE_EQ(0.0, guard.maxDrawdown());
    EXPECT_FALSE(guard.isHalted());
}

TEST(DrawdownGuard, RejectedSnapshotDoesNotArmTheNewDayBaseline)
{
    // The plausibility gate must run BEFORE the UTC-day roll, otherwise a bogus
    // first heartbeat of a new day seeds the daily baseline with a level the
    // account never held and the next genuine snapshot halts.
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0, 20000);
    guard.updateEquity(20000.0, 20001); // Implausible — rejected, no day roll.
    guard.updateEquity(9700.0, 20001);  // First real snapshot of the new day.

    EXPECT_DOUBLE_EQ(0.0, guard.dailyDrawdown());
    EXPECT_FALSE(guard.isHalted());
}

TEST(DrawdownGuard, NonFiniteSnapshotNeverDisablesTheBreaker)
{
    // std::from_chars accepts "nan"/"inf", so a REST payload can deliver one.
    // Accepting it as the first snapshot makes every later ratio NaN, and
    // NaN > threshold is always false — the breaker would silently die.
    DrawdownGuard guard(make_config(/*max_daily=*/0.02, /*max_dd=*/0.05));

    guard.updateEquity(std::numeric_limits<double>::quiet_NaN());
    guard.updateEquity(std::numeric_limits<double>::infinity());

    guard.updateEquity(10000.0);
    guard.updateEquity(9700.0); // 3% daily drop > 2% limit.

    EXPECT_NEAR(0.03, guard.dailyDrawdown(), 1e-9);
    EXPECT_TRUE(guard.isHalted());
}

TEST(DrawdownGuard, NonPositiveEquityBeforeAnyBaselineIsIgnored)
{
    DrawdownGuard guard(make_config(0.02, /*max_dd=*/0.05));

    guard.updateEquity(-100.0); // No baseline yet — cannot poison it.
    EXPECT_DOUBLE_EQ(0.0, guard.dailyDrawdown());

    guard.updateEquity(10000.0);
    guard.updateEquity(9700.0);
    EXPECT_TRUE(guard.isHalted());
}

TEST(DrawdownGuard, NonPositiveEquityAfterABaselineHalts)
{
    DrawdownGuard guard(make_config(0.10, /*max_dd=*/0.05));

    guard.updateEquity(10000.0);
    guard.updateEquity(-5.0); // A depleted account is a maximal drawdown.

    EXPECT_TRUE(guard.isHalted());
    EXPECT_EQ(ErrorCode::DrawdownLimitHit, guard.haltReason());
}

TEST(DrawdownEquity, NonFiniteLegMakesTheSnapshotIncomplete)
{
    DrawdownEquityInputs in;
    in.futures_total = std::numeric_limits<double>::infinity();
    in.cfd_equity = 10.0;

    const auto out = computeDrawdownEquity(in);

    EXPECT_FALSE(out.complete);
}

