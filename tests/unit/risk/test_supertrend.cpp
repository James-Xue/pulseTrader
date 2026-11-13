// test_supertrend.cpp — Unit tests for the engine-side SuperTrend helper
//
// The helper mirrors the Qt client's canonical Pine/Wilder formulation:
// SMA-seeded ATR, Wilder smoothing, ratcheted bands, and a trend flip on a
// close beyond the previous bar's opposite band.

#include "risk/SuperTrend.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace pulse::risk;

namespace
{

struct Series
{
    std::vector<double> highs;
    std::vector<double> lows;
    std::vector<double> closes;
};

/// A monotonically rising series; the last bar must be an uptrend.
Series risingSeries(std::size_t bars)
{
    Series series;
    for (std::size_t i = 0; i < bars; ++i)
    {
        const double base = 100.0 + static_cast<double>(i);
        series.highs.push_back(base + 1.0);
        series.lows.push_back(base - 1.0);
        series.closes.push_back(base + 0.5);
    }
    return series;
}

/// A monotonically falling series; the last bar must be a downtrend.
Series fallingSeries(std::size_t bars)
{
    Series series;
    for (std::size_t i = 0; i < bars; ++i)
    {
        const double base = 200.0 - static_cast<double>(i);
        series.highs.push_back(base + 1.0);
        series.lows.push_back(base - 1.0);
        series.closes.push_back(base - 0.5);
    }
    return series;
}

} // namespace

TEST(SuperTrend, InsufficientBarsReturnsNullopt)
{
    auto series = risingSeries(5);
    const auto point = SuperTrend::compute(series.highs, series.lows, series.closes, 10, 3.0);
    EXPECT_FALSE(point.has_value());
}

TEST(SuperTrend, ZeroPeriodReturnsNullopt)
{
    auto series = risingSeries(20);
    const auto point = SuperTrend::compute(series.highs, series.lows, series.closes, 0, 3.0);
    EXPECT_FALSE(point.has_value());
}

TEST(SuperTrend, MismatchedSeriesReturnsNullopt)
{
    auto series = risingSeries(20);
    series.lows.pop_back();
    const auto point = SuperTrend::compute(series.highs, series.lows, series.closes, 10, 3.0);
    EXPECT_FALSE(point.has_value());
}

TEST(SuperTrend, RisingSeriesIsBullishAndRidesLowerBand)
{
    auto series = risingSeries(40);
    const auto point = SuperTrend::compute(series.highs, series.lows, series.closes, 10, 3.0);
    ASSERT_TRUE(point.has_value());
    EXPECT_EQ(1, point->direction);
    EXPECT_GT(point->value, 0.0);
    // In an uptrend the SuperTrend line is a support level below the close.
    EXPECT_LE(point->value, series.closes.back());
}

TEST(SuperTrend, FallingSeriesIsBearishAndRidesUpperBand)
{
    auto series = fallingSeries(40);
    const auto point = SuperTrend::compute(series.highs, series.lows, series.closes, 10, 3.0);
    ASSERT_TRUE(point.has_value());
    EXPECT_EQ(-1, point->direction);
    EXPECT_GT(point->value, 0.0);
    // In a downtrend the SuperTrend line is a resistance level above the close.
    EXPECT_GE(point->value, series.closes.back());
}

TEST(SuperTrend, HigherMultiplierMovesTheBandFurtherAway)
{
    auto series = risingSeries(40);
    const auto tight = SuperTrend::compute(series.highs, series.lows, series.closes, 10, 1.0);
    const auto wide = SuperTrend::compute(series.highs, series.lows, series.closes, 10, 4.0);
    ASSERT_TRUE(tight.has_value());
    ASSERT_TRUE(wide.has_value());
    EXPECT_EQ(1, tight->direction);
    EXPECT_EQ(1, wide->direction);
    EXPECT_LT(wide->value, tight->value); // wider lower band sits lower
}

TEST(SuperTrend, MinimalWindowSeedsCanonicalDirection)
{
    // Exactly period + 1 bars: the first computable bar seeds the trend.
    // Canonical Pine starts "up" and flips down only when the close is below
    // the same bar's LOWER band. The old seed compared against the upper band
    // and returned -1 here, which the gold trader would treat as an immediate
    // exit against a long.
    auto series = risingSeries(11);
    const auto point = SuperTrend::compute(series.highs, series.lows, series.closes, 10, 3.0);
    ASSERT_TRUE(point.has_value());
    EXPECT_EQ(1, point->direction);
}
