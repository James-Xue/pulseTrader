// test_exchange_stop_plan.cpp — Unit tests for planExchangeStop (audit B1)

#include "execution/ExchangeStopPlan.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::execution;

// ---------------------------------------------------------------------------
// Basic plan shapes
// ---------------------------------------------------------------------------

TEST(ExchangeStopPlan, LongStopsBelowEntry)
{
    const auto plan = planExchangeStop(Side::Buy, 100.0, 0.01, 0.1, 100.0);
    ASSERT_TRUE(plan.has_value());
    EXPECT_NEAR(99.0, plan->trigger_price, 1e-9);
    EXPECT_EQ(2, plan->rule); // price <= trigger closes the long
    EXPECT_EQ("close-long-position", plan->order_type);
    EXPECT_TRUE(plan->is_long);
}

TEST(ExchangeStopPlan, ShortStopsAboveEntry)
{
    const auto plan = planExchangeStop(Side::Sell, 100.0, 0.01, 0.1, 100.0);
    ASSERT_TRUE(plan.has_value());
    EXPECT_NEAR(101.0, plan->trigger_price, 1e-9);
    EXPECT_EQ(1, plan->rule); // price >= trigger closes the short
    EXPECT_EQ("close-short-position", plan->order_type);
    EXPECT_FALSE(plan->is_long);
}

TEST(ExchangeStopPlan, RoundToContractTick)
{
    // 100.05 * 0.99 = 99.0495 → nearest 0.1 multiple = 99.0
    const auto plan = planExchangeStop(Side::Buy, 100.05, 0.01, 0.1, 100.05);
    ASSERT_TRUE(plan.has_value());
    EXPECT_NEAR(99.0, plan->trigger_price, 1e-9);

    // 0.1 tick with a 0.03% distance on 100.04 → 100.009988 → 100.0
    const auto tight = planExchangeStop(Side::Buy, 100.04, 0.0003, 0.1, 100.04);
    ASSERT_TRUE(tight.has_value());
    EXPECT_NEAR(100.0, tight->trigger_price, 1e-9);
}

// ---------------------------------------------------------------------------
// Breach rejection (Gate 1029 AUTO_TRIGGER_PRICE_LESS_LAST)
// ---------------------------------------------------------------------------

TEST(ExchangeStopPlan, RejectsLongStopAtOrAboveLast)
{
    // Entry 100, stop 99, but the market already fell to 98.
    EXPECT_FALSE(planExchangeStop(Side::Buy, 100.0, 0.01, 0.1, 98.0).has_value());

    // Stop exactly at the last price is also rejected by Gate.
    EXPECT_FALSE(planExchangeStop(Side::Buy, 100.0, 0.01, 0.1, 99.0).has_value());
}

TEST(ExchangeStopPlan, RejectsShortStopAtOrBelowLast)
{
    EXPECT_FALSE(planExchangeStop(Side::Sell, 100.0, 0.01, 0.1, 102.0).has_value());
    EXPECT_FALSE(planExchangeStop(Side::Sell, 100.0, 0.01, 0.1, 101.0).has_value());
}

TEST(ExchangeStopPlan, MissingLastPriceSkipsBreachCheck)
{
    const auto plan = planExchangeStop(Side::Buy, 100.0, 0.01, 0.1, 0.0);
    ASSERT_TRUE(plan.has_value());
    EXPECT_NEAR(99.0, plan->trigger_price, 1e-9);
}

// ---------------------------------------------------------------------------
// Invalid inputs
// ---------------------------------------------------------------------------

TEST(ExchangeStopPlan, RejectsUnknownTick)
{
    EXPECT_FALSE(planExchangeStop(Side::Buy, 100.0, 0.01, 0.0, 100.0).has_value());
    EXPECT_FALSE(planExchangeStop(Side::Buy, 100.0, 0.01, -0.1, 100.0).has_value());
}

TEST(ExchangeStopPlan, RejectsOutOfRangePct)
{
    EXPECT_FALSE(planExchangeStop(Side::Buy, 100.0, 0.0, 0.1, 100.0).has_value());
    EXPECT_FALSE(planExchangeStop(Side::Buy, 100.0, 1.0, 0.1, 100.0).has_value());
    EXPECT_FALSE(planExchangeStop(Side::Buy, 100.0, -0.01, 0.1, 100.0).has_value());
}

TEST(ExchangeStopPlan, RejectsNonPositiveEntry)
{
    EXPECT_FALSE(planExchangeStop(Side::Buy, 0.0, 0.01, 0.1, 0.0).has_value());
    EXPECT_FALSE(planExchangeStop(Side::Buy, -5.0, 0.01, 0.1, 0.0).has_value());
}