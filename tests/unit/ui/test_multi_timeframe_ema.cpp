// test_multi_timeframe_ema.cpp — unit tests for the multi-timeframe EMA panel.

#include "MultiTimeframeEma.hpp"

#include <gtest/gtest.h>

using namespace pulse::ui;

namespace
{

// Feed ticks at `stepMs` spacing; each lands in a new candle when stepMs
// equals the timeframe period.
void feedTicks(MultiTimeframeEma &panel, double start, double step,
               int n, std::int64_t &nowMs, std::int64_t stepMs)
{
    for (int i = 0; i < n; ++i)
    {
        panel.onTick(start + step * i, nowMs);
        nowMs += stepMs;
    }
}

std::int64_t alignedNow()
{
    // Divisible by 3'600'000 ms (60m) — hence by every timeframe bucket.
    return 1'699'999'200'000LL;
}

} // namespace

TEST(MultiTimeframeEmaTest, ConfigurationIsSevenBySeven)
{
    EXPECT_EQ(7, MultiTimeframeEma::timeframes().size());
    EXPECT_EQ(7, MultiTimeframeEma::periods().size());
    // Display order and exact periods.
    EXPECT_EQ(QStringLiteral("1m"), MultiTimeframeEma::timeframes()[0]);
    EXPECT_EQ(QStringLiteral("60m"), MultiTimeframeEma::timeframes()[6]);
    const auto &p = MultiTimeframeEma::periods();
    EXPECT_EQ(7, p[0]);
    EXPECT_EQ(14, p[1]);
    EXPECT_EQ(31, p[2]);
    EXPECT_EQ(60, p[3]);
    EXPECT_EQ(90, p[4]);
    EXPECT_EQ(120, p[5]);
    EXPECT_EQ(180, p[6]);
}

TEST(MultiTimeframeEmaTest, WarmupRequiresOneHundredEighty)
{
    MultiTimeframeEma panel;
    std::int64_t now = alignedNow();
    feedTicks(panel, 4400.0, 1.0, 100, now, 60'000);  // 100 candles — below 180.
    const auto rows = panel.snapshot();
    ASSERT_EQ(7, rows.size());
    EXPECT_EQ(QStringLiteral("1m"), rows[0].period);
    EXPECT_FALSE(rows[0].valid);
    EXPECT_EQ(7, rows[0].emas.size());
}

TEST(MultiTimeframeEmaTest, RisingSeriesOrdersEmas)
{
    MultiTimeframeEma panel;
    std::int64_t now = alignedNow();
    // 240 rising 1m candles: EMA7 (fast) must sit above EMA180 (slow).
    feedTicks(panel, 4400.0, 0.5, 240, now, 60'000);
    const auto rows = panel.snapshot();
    ASSERT_EQ(7, rows.size());
    ASSERT_TRUE(rows[0].valid);
    ASSERT_EQ(7, rows[0].emas.size());
    EXPECT_GT(rows[0].emas[0], rows[0].emas[6]);  // EMA7 > EMA180 on an uptrend.
    // All EMA values strictly descending in the stack (7 > 14 > ... > 180).
    for (std::size_t i = 1; i < rows[0].emas.size(); ++i)
    {
        EXPECT_GT(rows[0].emas[i - 1], rows[0].emas[i]);
    }
}

TEST(MultiTimeframeEmaTest, FallingSeriesReversesOrder)
{
    MultiTimeframeEma panel;
    std::int64_t now = alignedNow();
    feedTicks(panel, 4600.0, -0.5, 240, now, 60'000);
    const auto rows = panel.snapshot();
    ASSERT_TRUE(rows[0].valid);
    EXPECT_LT(rows[0].emas[0], rows[0].emas[6]);  // EMA7 < EMA180 on a downtrend.
}

TEST(MultiTimeframeEmaTest, TimeframeBucketsIndependent)
{
    MultiTimeframeEma panel;
    std::int64_t now = alignedNow();
    // 240 ticks at 1m: 1m track has 240 candles, 3m has 80, 30m has 8.
    feedTicks(panel, 4400.0, 0.0, 240, now, 60'000);
    EXPECT_EQ(240, panel.candleCount(0));
    EXPECT_EQ(80, panel.candleCount(1));   // 3m
    EXPECT_EQ(8, panel.candleCount(5));    // 30m
    EXPECT_EQ(4, panel.candleCount(6));    // 60m

    const auto rows = panel.snapshot();
    ASSERT_EQ(7, rows.size());
    EXPECT_TRUE(rows[0].valid);    // 1m has 240 ≥ 180.
    EXPECT_FALSE(rows[5].valid);   // 30m has 8 < 180.
}
