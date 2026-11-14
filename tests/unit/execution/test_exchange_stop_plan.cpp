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

// ---------------------------------------------------------------------------
// Existing trigger classification (ensureFuturesStops floor detection)
// ---------------------------------------------------------------------------

namespace
{

nlohmann::json priceOrder(const std::string &order_type, int rule,
                          const std::string &price)
{
    return nlohmann::json{
        { "id", "2088850508826546176" },
        { "status", "open" },
        { "order_type", order_type },
        { "trigger",
          nlohmann::json{
              { "price", price },
              { "rule", rule },
              { "price_type", 0 },
              { "expiration", 0 },
          } },
    };
}

} // namespace

TEST(ExchangeStopPlan, AcceptsLossSideStopForLongAndShort)
{
    // Long: close-long-position, rule 2 (price falls to/below), 1% below entry.
    EXPECT_TRUE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 2, "59400"), Side::Buy, 60000.0,
        0.01, 0.1));
    // Short: close-short-position, rule 1 (price rises to/above), 1% above.
    EXPECT_TRUE(isProtectiveStopTrigger(
        priceOrder("close-short-position", 1, "60600"), Side::Sell, 60000.0,
        0.01, 0.1));
}

TEST(ExchangeStopPlan, RejectsTakeProfitShapes)
{
    // A long take-profit sits ABOVE entry. Whatever the rule, it is not on
    // the loss side of the entry, so it never counts as the floor.
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 2, "60500"), Side::Buy, 60000.0,
        0.01, 0.1));
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 1, "60500"), Side::Buy, 60000.0,
        0.01, 0.1));

    // Short take-profit: BELOW entry.
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-short-position", 1, "59500"), Side::Sell, 60000.0,
        0.01, 0.1));
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-short-position", 2, "59500"), Side::Sell, 60000.0,
        0.01, 0.1));
}

TEST(ExchangeStopPlan, RejectsOppositeSideAndOpenOrderTriggers)
{
    // A short's stop does not protect a long, and vice versa.
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-short-position", 1, "60600"), Side::Buy, 60000.0,
        0.01, 0.1));
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 2, "59400"), Side::Sell, 60000.0,
        0.01, 0.1));
    // Opening orders are never a protection floor.
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("open-long-position", 2, "59400"), Side::Buy, 60000.0,
        0.01, 0.1));
}

TEST(ExchangeStopPlan, RejectsStopsOnTheWrongSideOfThisEntry)
{
    // The trigger must be on the LOSS side of THIS entry. A long stop above
    // the current entry (or a short stop below it) belongs to a different
    // position and would fire immediately or never.
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 2, "70500"), Side::Buy, 70000.0,
        0.01, 0.1));
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-short-position", 1, "69500"), Side::Sell, 70000.0,
        0.01, 0.1));
}

TEST(ExchangeStopPlan, RejectsAStaleStopPricedOffAnotherEntry)
{
    // A leftover 59,400 stop while the current long entered at 70,000 is on
    // the loss side but 15% away — far beyond the 1% catastrophe distance, so
    // it must not be adopted as this position's floor.
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 2, "59400"), Side::Buy, 70000.0,
        0.01, 0.1));
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-short-position", 1, "80600"), Side::Sell, 70000.0,
        0.01, 0.1));

    // At the configured distance the trigger IS the floor.
    EXPECT_TRUE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 2, "69300"), Side::Buy, 70000.0,
        0.01, 0.1));
}

TEST(ExchangeStopPlan, AllowsHalfATickOfRoundingSlack)
{
    // planExchangeStop rounds to the nearest tick: entry 100.05 at 1% -> raw
    // 99.0495 -> 99.0, which is slightly farther than the raw level. The
    // engine's own trigger must still classify as protective, or every
    // revalidation pass would place a duplicate.
    EXPECT_TRUE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 2, "99.0"), Side::Buy, 100.05, 0.01,
        0.1));
    // One tick further than the slack allows is rejected.
    EXPECT_FALSE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 2, "98.9"), Side::Buy, 100.05, 0.01,
        0.1));
}

TEST(ExchangeStopPlan, UnknownTickDisablesTheDistanceBound)
{
    // The contract price unit could not be resolved, so the level cannot be
    // verified. Any loss-side stop is better than none: adopt it rather than
    // declaring the position naked (a new trigger could not be planned
    // anyway).
    EXPECT_TRUE(isProtectiveStopTrigger(
        priceOrder("close-long-position", 2, "59400"), Side::Buy, 70000.0,
        0.01, 0.0));
}

TEST(ExchangeStopPlan, RejectsMalformedTriggerObjects)
{
    EXPECT_FALSE(isProtectiveStopTrigger(nlohmann::json::array(), Side::Buy,
                                        100.0, 0.01, 0.1));
    EXPECT_FALSE(isProtectiveStopTrigger(
        nlohmann::json{ { "order_type", "close-long-position" } }, Side::Buy,
        100.0, 0.01, 0.1));
    EXPECT_FALSE(isProtectiveStopTrigger(
        nlohmann::json{ { "order_type", "close-long-position" },
                        { "trigger", nlohmann::json{ { "rule", 2 } } } },
        Side::Buy, 100.0, 0.01, 0.1));
    EXPECT_FALSE(isProtectiveStopTrigger(
        nlohmann::json{ { "order_type", "close-long-position" },
                        { "trigger", nlohmann::json{ { "price", "99" },
                                                     { "rule", 0 } } } },
        Side::Buy, 100.0, 0.01, 0.1));
    EXPECT_FALSE(isProtectiveStopTrigger(
        nlohmann::json{ { "order_type", "close-long-position" },
                        { "trigger", nlohmann::json{ { "price", "abc" },
                                                     { "rule", 2 } } } },
        Side::Buy, 100.0, 0.01, 0.1));
    // A numeric price (locally built object) is classified the same way.
    EXPECT_TRUE(isProtectiveStopTrigger(
        nlohmann::json{ { "order_type", "close-long-position" },
                        { "trigger", nlohmann::json{ { "price", 99 },
                                                     { "rule", 2 } } } },
        Side::Buy, 100.0, 0.01, 0.1));
}