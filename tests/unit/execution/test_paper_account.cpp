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
