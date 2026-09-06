// test_funding_watch.cpp — funding-window decision logic (pure, no network)

#include "funding/FundingWatch.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

namespace pulse::funding
{

// ---------------------------------------------------------------------------
// fundingWindowOpen
// ---------------------------------------------------------------------------

TEST(FundingWindowOpen, EmptyNeverOpen)
{
    EXPECT_FALSE(fundingWindowOpen({}, 0.0005, 6));
}

TEST(FundingWindowOpen, InsufficientHistoryNeverOpen)
{
    // Only 3 applies on record — cannot prove 6 sustained events.
    EXPECT_FALSE(fundingWindowOpen({ 0.001, 0.001, 0.001 }, 0.0005, 6));
    EXPECT_FALSE(fundingWindowOpen({ 0.001, 0.001 }, 0.0005, 3));
}

TEST(FundingWindowOpen, SustainedHighOpens)
{
    // Newest-first: 8 applies, all above a 5bp threshold.
    const std::vector<double> rates = {
        0.0008, 0.0009, 0.0012, 0.0010, 0.0009, 0.0008, 0.0007, 0.0006
    };
    EXPECT_TRUE(fundingWindowOpen(rates, 0.0005, 6));
}

TEST(FundingWindowOpen, SingleLowestInWindowBreaksIt)
{
    // The most recent 6 all qualify except apply #4 (0.0004 ≤ threshold) —
    // order matters: the check covers the LAST consec events only.
    const std::vector<double> rates = {
        0.0008, 0.0009, 0.0004, 0.0010, 0.0009, 0.0008, 0.0007, 0.0006
    };
    EXPECT_FALSE(fundingWindowOpen(rates, 0.0005, 6));
    // Breach sits at index 2 (4th newest) — a 3-event window does not see it.
    const std::vector<double> rates3 = {
        0.0008, 0.0009, 0.0010, 0.0004, 0.0009, 0.0008, 0.0007, 0.0006
    };
    EXPECT_FALSE(fundingWindowOpen(rates3, 0.0005, 6));
    EXPECT_TRUE(fundingWindowOpen(rates3, 0.0005, 3));
}

TEST(FundingWindowOpen, OldLowestIgnored)
{
    // The breach sits 7 applies back; a 6-event window must not see it.
    const std::vector<double> rates = {
        0.0008, 0.0009, 0.0012, 0.0010, 0.0009, 0.0008, 0.0001, 0.0006
    };
    EXPECT_TRUE(fundingWindowOpen(rates, 0.0005, 6));
}

TEST(FundingWindowOpen, ThresholdIsStrictlyGreater)
{
    // Rate exactly AT the threshold is not above it (noise-zone boundary).
    const std::vector<double> rates = { 0.0005, 0.0005, 0.0005, 0.0005,
                                        0.0005, 0.0005 };
    EXPECT_FALSE(fundingWindowOpen(rates, 0.0005, 6));
    EXPECT_TRUE(fundingWindowOpen(rates, 0.0004, 6));
}

TEST(FundingWindowOpen, NegativeRatesNeverOpen)
{
    const std::vector<double> rates = { -0.0001, 0.0008, 0.0008, 0.0008,
                                        0.0008, 0.0008 };
    EXPECT_FALSE(fundingWindowOpen(rates, 0.0005, 6));
}

// ---------------------------------------------------------------------------
// fundingRatesFromJson
// ---------------------------------------------------------------------------

TEST(FundingRatesFromJson, ParsesGateShapeNewestFirst)
{
    const nlohmann::json body = nlohmann::json::array({
        { { "r", "0.0008" }, { "t", 1788681602 } },
        { { "r", "-0.0001" }, { "t", 1788652802 } },
        { { "r", "0.0005" }, { "t", 1788624002 } },
    });
    const auto rates = fundingRatesFromJson(body, 10);
    ASSERT_EQ(rates.size(), 3u);
    EXPECT_DOUBLE_EQ(rates[0], 0.0008);
    EXPECT_DOUBLE_EQ(rates[1], -0.0001);
    EXPECT_DOUBLE_EQ(rates[2], 0.0005);
}

TEST(FundingRatesFromJson, CapsAtMax)
{
    nlohmann::json body = nlohmann::json::array();
    for (int i = 0; i < 20; ++i)
    {
        body.push_back({ { "r", "0.001" }, { "t", 1788000000 + i * 28800 } });
    }
    EXPECT_EQ(fundingRatesFromJson(body, 12).size(), 12u);
}

TEST(FundingRatesFromJson, NonArrayOrMalformedYieldsEmptyOrSkips)
{
    EXPECT_TRUE(fundingRatesFromJson(nlohmann::json{ 42 }, 10).empty());
    const nlohmann::json body = nlohmann::json::array({
        { { "r", "0.0008" }, { "t", 1788681602 } },
        { { "t", 1788652802 } },              // missing "r" → skipped
        { { "r", "not-a-number" }, { "t", 0 } }, // unparsable → skipped
    });
    const auto rates = fundingRatesFromJson(body, 10);
    ASSERT_EQ(rates.size(), 1u);
    EXPECT_DOUBLE_EQ(rates[0], 0.0008);
}

} // namespace pulse::funding
