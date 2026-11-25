// test_fill_math.cpp — Golden-value tests for the shared fill arithmetic.
//
// The numbers below were captured from BacktestAccount's pre-refactor formulas
// (2026-11-25) and are asserted bit-identically (EXPECT_DOUBLE_EQ, not an
// epsilon): the whole point of src/core/fill_math.hpp is that the offline
// backtest and the live paper venue can never drift numerically.

#include "core/fill_math.hpp"

#include <gtest/gtest.h>

using namespace pulse;

namespace
{

// ---------------------------------------------------------------------------
// feeFor — tri-state rate semantics
// ---------------------------------------------------------------------------

TEST(FillMathTest, FuturesDefaultTakerRate_IsFiveBasisPoints)
{
    // 20 contracts × 2000 × quanto 0.01 = 400 notional × 0.0005 = 0.2
    EXPECT_DOUBLE_EQ(0.2, core::fill_math::feeFor(
                              20.0, 2000.0, 0.01, MarketType::Futures, 0.0));
}

TEST(FillMathTest, SpotDefaultTakerRate_IsTenBasisPoints)
{
    // 2 × 100 × 1 = 200 notional × 0.001 = 0.2
    EXPECT_DOUBLE_EQ(0.2, core::fill_math::feeFor(
                              2.0, 100.0, 1.0, MarketType::Spot, 0.0));
}

TEST(FillMathTest, ExplicitRate_OverridesMarketDefault)
{
    // 1 lot × 4000 × quanto 100 = 400,000 notional × 0.0002 maker = 80.
    EXPECT_DOUBLE_EQ(80.0, core::fill_math::feeFor(
                               1.0, 4000.0, 100.0, MarketType::Cfd, 0.0002));
}

TEST(FillMathTest, NegativeRate_DisablesFees)
{
    EXPECT_DOUBLE_EQ(0.0, core::fill_math::feeFor(
                              1.0, 4000.0, 100.0, MarketType::Cfd, -1.0));
}

// ---------------------------------------------------------------------------
// positionPnl — long and short mirror
// ---------------------------------------------------------------------------

TEST(FillMathTest, LongPnl_IsMarkMinusEntry)
{
    // (4010 - 4000) × 1 × 100 = 1000
    EXPECT_DOUBLE_EQ(1000.0, core::fill_math::positionPnl(
                                 Side::Buy, 4000.0, 4010.0, 1.0, 100.0));
}

TEST(FillMathTest, ShortPnl_IsEntryMinusMark)
{
    // (4000 - 3990) × 0.5 × 100 = 500
    EXPECT_DOUBLE_EQ(500.0, core::fill_math::positionPnl(
                                Side::Sell, 4000.0, 3990.0, 0.5, 100.0));
}

TEST(FillMathTest, LongLoss_IsNegative)
{
    EXPECT_DOUBLE_EQ(-250.0, core::fill_math::positionPnl(
                                 Side::Buy, 4000.0, 3997.5, 1.0, 100.0));
}

// ---------------------------------------------------------------------------
// requiredMargin
// ---------------------------------------------------------------------------

TEST(FillMathTest, Margin_IsNotionalOverLeverage)
{
    EXPECT_DOUBLE_EQ(20000.0, core::fill_math::requiredMargin(400000.0, 20.0));
    EXPECT_DOUBLE_EQ(400000.0, core::fill_math::requiredMargin(400000.0, 1.0));
}

TEST(FillMathTest, NonPositiveLeverage_ReturnsZeroNotInfinity)
{
    // A division-by-zero here would poison an account ledger; the shared
    // helper returns 0 and leaves the policy decision to the caller.
    EXPECT_DOUBLE_EQ(0.0, core::fill_math::requiredMargin(400000.0, 0.0));
    EXPECT_DOUBLE_EQ(0.0, core::fill_math::requiredMargin(400000.0, -5.0));
}

// ---------------------------------------------------------------------------
// Whole-trade sanity: one ETH futures round trip (same numbers the backtest
// account test asserts, so a drift in either implementation fails here).
// ---------------------------------------------------------------------------

TEST(FillMathTest, EthRoundTrip_MatchesBacktestGoldenValues)
{
    constexpr double qty = 20.0;
    constexpr double quanto = 0.01;
    constexpr double entry = 2000.0;
    constexpr double exit = 2050.0;

    const double entry_fee = core::fill_math::feeFor(
        qty, entry, quanto, MarketType::Futures, 0.0005);
    const double exit_fee = core::fill_math::feeFor(
        qty, exit, quanto, MarketType::Futures, 0.0005);
    const double gross = core::fill_math::positionPnl(
        Side::Buy, entry, exit, qty, quanto);

    EXPECT_DOUBLE_EQ(0.2, entry_fee);
    EXPECT_DOUBLE_EQ(0.205, exit_fee);
    EXPECT_DOUBLE_EQ(10.0, gross);
    EXPECT_DOUBLE_EQ(10.0 - 0.2 - 0.205, gross - entry_fee - exit_fee);
}

} // namespace
