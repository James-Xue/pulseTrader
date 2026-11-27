// test_paper_account.cpp — Simulated ledger arithmetic (pure memory).

#include "execution/PaperAccount.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace pulse;
using namespace pulse::execution;

namespace
{

PaperConfig makePaperConfig()
{
    PaperConfig cfg;
    cfg.account_id = "test-paper";
    cfg.initial_balance_usdt = 10'000.0;
    return cfg;
}

} // namespace

TEST(PaperAccountTest, SeedsEveryLegIndependently)
{
    PaperAccount account(makePaperConfig());
    const auto s = account.snapshot(0.0);

    EXPECT_EQ("test-paper", s.account_id);
    EXPECT_DOUBLE_EQ(10'000.0, s.spot.cash_usdt);
    EXPECT_DOUBLE_EQ(10'000.0, s.futures.cash_usdt);
    EXPECT_DOUBLE_EQ(10'000.0, s.cfd.cash_usdt);
    EXPECT_DOUBLE_EQ(30'000.0, s.equity_usdt);
    EXPECT_DOUBLE_EQ(30'000.0, s.peak_equity_usdt);
}

TEST(PaperAccountTest, LockMarginMovesCashToLocked)
{
    PaperAccount account(makePaperConfig());

    ASSERT_TRUE(account.lockMargin(PaperLeg::Cfd, 2'000.0));
    const auto s = account.snapshot(0.0);
    EXPECT_DOUBLE_EQ(8'000.0, s.cfd.cash_usdt);
    EXPECT_DOUBLE_EQ(2'000.0, s.cfd.locked_margin_usdt);
    // Equity is unchanged: the margin is still the account's money.
    EXPECT_DOUBLE_EQ(30'000.0, s.equity_usdt);
}

TEST(PaperAccountTest, LockMarginRefusesInsufficientWithoutChangingState)
{
    PaperAccount account(makePaperConfig());

    EXPECT_FALSE(account.lockMargin(PaperLeg::Spot, 10'000.01));
    const auto s = account.snapshot(0.0);
    EXPECT_DOUBLE_EQ(10'000.0, s.spot.cash_usdt);
    EXPECT_DOUBLE_EQ(0.0, s.spot.locked_margin_usdt);
}

TEST(PaperAccountTest, LockMarginAcceptsExactBalance)
{
    PaperAccount account(makePaperConfig());
    // Binary floating point: exactly-the-balance must still be affordable.
    EXPECT_TRUE(account.lockMargin(PaperLeg::Futures, 10'000.0));
    EXPECT_DOUBLE_EQ(0.0, account.snapshot(0.0).futures.cash_usdt);
}

TEST(PaperAccountTest, ReleaseMarginReturnsMarginAndRealizedPnl)
{
    PaperAccount account(makePaperConfig());
    ASSERT_TRUE(account.lockMargin(PaperLeg::Cfd, 2'000.0));

    // Close with +250 gross PnL.
    account.releaseMargin(PaperLeg::Cfd, 2'000.0, 250.0);

    const auto s = account.snapshot(0.0);
    // 10,000 - 2,000 (locked) + 2,000 (released) + 250 (PnL) = 10,250.
    EXPECT_DOUBLE_EQ(10'250.0, s.cfd.cash_usdt);
    EXPECT_DOUBLE_EQ(0.0, s.cfd.locked_margin_usdt);
    EXPECT_DOUBLE_EQ(250.0, s.cfd.realized_pnl_usdt);
    EXPECT_DOUBLE_EQ(30'250.0, s.equity_usdt);
}

TEST(PaperAccountTest, ReleaseMarginNeverMintsMoreThanLocked)
{
    PaperAccount account(makePaperConfig());
    ASSERT_TRUE(account.lockMargin(PaperLeg::Cfd, 1'000.0));

    // A mismatched release (bug or corrupt state) must not create cash out of
    // thin air: it releases at most what is locked.
    account.releaseMargin(PaperLeg::Cfd, 5'000.0, 0.0);

    const auto s = account.snapshot(0.0);
    EXPECT_DOUBLE_EQ(10'000.0, s.cfd.cash_usdt);
    EXPECT_DOUBLE_EQ(0.0, s.cfd.locked_margin_usdt);
}

TEST(PaperAccountTest, ChargeFeeDebitsCashAndAccumulates)
{
    PaperAccount account(makePaperConfig());
    account.chargeFee(PaperLeg::Cfd, 6.0);
    account.chargeFee(PaperLeg::Cfd, 6.0);

    const auto s = account.snapshot(0.0);
    EXPECT_DOUBLE_EQ(9'988.0, s.cfd.cash_usdt);
    EXPECT_DOUBLE_EQ(12.0, s.cfd.fees_paid_usdt);
}

TEST(PaperAccountTest, UnrealizedPnlFlowsIntoEquityOnly)
{
    PaperAccount account(makePaperConfig());
    const auto s = account.snapshot(-125.0);
    EXPECT_DOUBLE_EQ(29'875.0, s.equity_usdt);
    EXPECT_DOUBLE_EQ(10'000.0, s.cfd.cash_usdt); // cash untouched by marks
}

TEST(PaperAccountTest, PeakTracksHighWaterMarkAndCanBeSeeded)
{
    PaperAccount account(makePaperConfig());
    account.updateEquityPeak(31'000.0);
    account.updateEquityPeak(30'500.0); // lower mark must not lower the peak
    EXPECT_DOUBLE_EQ(31'000.0, account.peakEquity());

    account.seedEquityBaselines(32'000.0, 30'100.0);
    EXPECT_DOUBLE_EQ(32'000.0, account.peakEquity());
    EXPECT_DOUBLE_EQ(30'100.0, account.dailyStartEquity());

    account.resetDailyBaseline(30'400.0);
    EXPECT_DOUBLE_EQ(30'400.0, account.dailyStartEquity());
}

TEST(PaperAccountTest, JsonRoundTripPreservesEveryLeg)
{
    PaperAccount account(makePaperConfig());
    ASSERT_TRUE(account.lockMargin(PaperLeg::Cfd, 2'000.0));
    account.chargeFee(PaperLeg::Cfd, 6.0);
    account.releaseMargin(PaperLeg::Spot, 0.0, 0.0);
    account.updateEquityPeak(30'500.0);

    const nlohmann::json j = account.toJson();

    PaperConfig other_cfg = makePaperConfig();
    other_cfg.initial_balance_usdt = 1.0; // must be overwritten by restore()
    PaperAccount restored(other_cfg);
    ASSERT_TRUE(restored.restore(j));

    const auto a = account.snapshot(0.0);
    const auto b = restored.snapshot(0.0);
    EXPECT_DOUBLE_EQ(a.equity_usdt, b.equity_usdt);
    EXPECT_DOUBLE_EQ(a.cfd.cash_usdt, b.cfd.cash_usdt);
    EXPECT_DOUBLE_EQ(a.cfd.locked_margin_usdt, b.cfd.locked_margin_usdt);
    EXPECT_DOUBLE_EQ(a.cfd.fees_paid_usdt, b.cfd.fees_paid_usdt);
    EXPECT_DOUBLE_EQ(a.peak_equity_usdt, b.peak_equity_usdt);
}

TEST(PaperAccountTest, RestoreRejectsMalformedPayload)
{
    PaperAccount account(makePaperConfig());
    EXPECT_FALSE(account.restore(nlohmann::json::object()));
    EXPECT_FALSE(account.restore(nlohmann::json{ { "account_id", "x" } }));
}

TEST(PaperAccountTest, LegMappingMatchesMarketType)
{
    EXPECT_EQ(PaperLeg::Spot, legFor(MarketType::Spot));
    EXPECT_EQ(PaperLeg::Futures, legFor(MarketType::Futures));
    EXPECT_EQ(PaperLeg::Cfd, legFor(MarketType::Cfd));
}

// ---------------------------------------------------------------------------
// P4 of the 2026-11-27 paper-team reconciliation audit: capital events and
// the account identity.
// ---------------------------------------------------------------------------

TEST(PaperAccountTest, ClosedRoundTripIdentity)
{
    // cash + locked == deposits + realized - fees after a full CFD round trip.
    PaperConfig cfg;
    cfg.initial_balance_usdt = 100.0;
    PaperAccount account(cfg);

    const double margin = 0.01 * 100.0 * 4000.0 / 1000.0; // 4.0 at 1000x
    ASSERT_TRUE(account.lockMargin(PaperLeg::Cfd, margin));
    account.chargeFee(PaperLeg::Cfd, 0.06); // entry BUY leg
    account.releaseMargin(PaperLeg::Cfd, margin, 10.0); // exit at 4010
    account.chargeFee(PaperLeg::Cfd, 0.0); // SELL leg is free

    const auto s = account.snapshot();
    EXPECT_NEAR(109.94, s.cfd.cash_usdt, 1e-9);
    EXPECT_NEAR(0.0, s.cfd.locked_margin_usdt, 1e-9);
    EXPECT_NEAR(0.0, account.identityResidual(PaperLeg::Cfd), 1e-9);
}

TEST(PaperAccountTest, CapitalEventsKeepTheIdentity)
{
    PaperConfig cfg;
    cfg.initial_balance_usdt = 100.0;
    PaperAccount account(cfg);

    account.applyCapital(PaperLeg::Cfd, "topup", 50.0, "test top-up", 1);
    EXPECT_NEAR(150.0, account.netDeposits(PaperLeg::Cfd), 1e-9);
    EXPECT_NEAR(0.0, account.identityResidual(PaperLeg::Cfd), 1e-9);

    account.applyCapital(PaperLeg::Cfd, "withdraw", 20.0, "test withdraw", 2);
    EXPECT_NEAR(130.0, account.netDeposits(PaperLeg::Cfd), 1e-9);
    EXPECT_NEAR(0.0, account.identityResidual(PaperLeg::Cfd), 1e-9);

    // A reset opens a new epoch: cash = new deposit, counters zeroed.
    account.applyCapital(PaperLeg::Cfd, "reset", 40.0, "reset", 3);
    const auto s = account.snapshot();
    EXPECT_NEAR(40.0, s.cfd.cash_usdt, 1e-9);
    EXPECT_NEAR(0.0, s.cfd.realized_pnl_usdt, 1e-9);
    EXPECT_NEAR(0.0, s.cfd.fees_paid_usdt, 1e-9);
    EXPECT_NEAR(0.0, account.identityResidual(PaperLeg::Cfd), 1e-9);
    EXPECT_EQ(3u, account.capitalEvents().size());
}

TEST(PaperAccountTest, ResetRefusedWhileMarginIsLocked)
{
    PaperConfig cfg;
    cfg.initial_balance_usdt = 100.0;
    PaperAccount account(cfg);
    ASSERT_TRUE(account.lockMargin(PaperLeg::Cfd, 4.0));

    const auto refused = account.applyCapital(PaperLeg::Cfd, "reset", 50.0);
    EXPECT_TRUE(refused.id.empty());
    // Locked margin is unchanged and still backed by the same cash.
    const auto s = account.snapshot();
    EXPECT_NEAR(96.0, s.cfd.cash_usdt, 1e-9);
    EXPECT_NEAR(4.0, s.cfd.locked_margin_usdt, 1e-9);
    EXPECT_TRUE(account.capitalEvents().empty());
}

TEST(PaperAccountTest, LegacyAdjustmentsAreIdempotent)
{
    PaperConfig cfg;
    cfg.initial_balance_usdt = 100.0;
    PaperAccount account(cfg);

    EXPECT_EQ(4, seedLegacyReconciliationAdjustments(account, 1));
    EXPECT_EQ(0, seedLegacyReconciliationAdjustments(account, 2));

    const auto events = account.capitalEvents();
    ASSERT_EQ(4u, events.size());
    EXPECT_EQ("legacy:E0", events[0].id);
    EXPECT_NEAR(-5.91, events[0].realized_delta, 1e-9);
    EXPECT_NEAR(-10.07, events[1].realized_delta, 1e-9);
    EXPECT_NEAR(-0.54, events[1].fees_delta, 1e-9);
    EXPECT_NEAR(-0.06, events[2].fees_delta, 1e-9);
    EXPECT_NEAR(0.0, events[3].realized_delta, 1e-9);
    // Adjustments document history; they never move cash.
    EXPECT_NEAR(100.0, account.snapshot().cfd.cash_usdt, 1e-9);
}
