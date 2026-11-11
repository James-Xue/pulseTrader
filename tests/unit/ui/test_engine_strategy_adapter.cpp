// test_engine_strategy_adapter.cpp — unit tests for the multi-timeframe
// SuperTrend adapter (tick → candles → SuperTrendScalper evaluateState).
//
// No network: warmup() is not exercised here (it needs a REST client); the
// candle aggregation, SuperTrend state evaluation and flip detection are pure.

#include "EngineStrategyAdapter.hpp"

#include "market/KlineBuffer.hpp"  // pulse::market::Kline complete type

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

using namespace pulse::ui;

namespace
{

// Feed ticks spaced `stepMs` apart (each lands in a new candle of that
// timeframe bucket size when stepMs == periodMs).
void feedTicks(EngineStrategyAdapter &adapter, double start, double step,
               int n, std::int64_t &nowMs, std::int64_t stepMs)
{
    for (int i = 0; i < n; ++i)
    {
        adapter.onTick(start + step * i, nowMs, 1.5, 1.0, 60'000);
        nowMs += stepMs;
    }
}

// Aligned to a 10s boundary so 10s-bucket arithmetic is exact.
std::int64_t alignedNow()
{
    return 1'700'000'040'000LL;
}

} // namespace

TEST(EngineStrategyAdapterTest, TimeframesFixedOrderAndCount)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = alignedNow();
    // 1m candle only (12 ticks at 60s) — 10s/15m/30m stay empty.
    feedTicks(adapter, 4400.0, 1.0, 12, now, 60'000);
    EXPECT_EQ(12, adapter.klineCount());  // 1m track.

    const auto outcome = adapter.onTick(4412.0, now, 1.5, 1.0, 60'000);
    ASSERT_EQ(4, outcome.trends.size());
    EXPECT_EQ(QStringLiteral("10s"), outcome.trends[0].label);
    EXPECT_EQ(QStringLiteral("1m"), outcome.trends[1].label);
    EXPECT_EQ(QStringLiteral("15m"), outcome.trends[2].label);
    EXPECT_EQ(QStringLiteral("30m"), outcome.trends[3].label);
}

TEST(EngineStrategyAdapterTest, WarmupGateOnEachTimeframe)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = alignedNow();
    // 11 ticks at 10s spacing with a small drift (ATR > 0 so a state can
    // form): the 10s timeframe gets 11 candles (warmed + valid), the 1m
    // timeframe only 2 (still warming).
    feedTicks(adapter, 4400.0, 0.1, 11, now, 10'000);
    const auto outcome = adapter.onTick(4401.1, now, 5.0, 5.0, 60'000);
    EXPECT_TRUE(outcome.trends[0].valid);
    EXPECT_FALSE(outcome.trends[1].valid);
    EXPECT_FALSE(outcome.decision.has_value());
}

TEST(EngineStrategyAdapterTest, BearToBullFlipOpensBuy)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = alignedNow();
    // 1. Flat warm-up (12 candles at 1m — 1m track ready). ATR is zero on a
    //    flat market, so no state is committed yet.
    feedTicks(adapter, 4400.0, 0.0, 12, now, 60'000);
    // 2. Down-trend: the first volatile evaluation commits the bearish
    //    state (no flip on the initial pass).
    for (int i = 0; i < 8; ++i)
    {
        adapter.onTick(4398.0 - 2.0 * i, now, 5.0, 5.0, 60'000);
        now += 60'000;
    }
    // 3. Up-turn: close crosses above the upper band → bear→bull flip →
    //    LONG entry.
    bool opened = false;
    pulse::ui::AdapterDecision first;
    for (int i = 1; i <= 15; ++i)
    {
        const auto outcome = adapter.onTick(4382.0 + 3.0 * i, now, 5.0, 5.0, 60'000);
        now += 60'000;
        if (outcome.decision.has_value() && outcome.decision->shouldOpen)
        {
            opened = true;
            first = *outcome.decision;
            break;
        }
    }
    ASSERT_TRUE(opened) << "no Buy fired on the bear→bull flip";
    EXPECT_EQ(QStringLiteral("long"), first.side);
    EXPECT_GT(first.takeProfit, first.entryPrice);   // long TP above entry.
    EXPECT_LT(first.stopLoss, first.entryPrice);
    EXPECT_GT(first.confidence, 0.0);
}

TEST(EngineStrategyAdapterTest, BullToBearFlipOpensShort)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = alignedNow();
    feedTicks(adapter, 4400.0, 0.0, 12, now, 60'000);
    // Up-trend first (commits bullish), then a reversal down → short entry.
    feedTicks(adapter, 4402.0, 2.0, 8, now, 60'000);
    bool opened = false;
    pulse::ui::AdapterDecision first;
    for (int i = 1; i <= 15; ++i)
    {
        const auto outcome = adapter.onTick(4418.0 - 3.0 * i, now, 5.0, 5.0, 60'000);
        now += 60'000;
        if (outcome.decision.has_value() && outcome.decision->shouldOpen)
        {
            opened = true;
            first = *outcome.decision;
            break;
        }
    }
    ASSERT_TRUE(opened) << "no Sell fired on the bull→bear flip";
    EXPECT_EQ(QStringLiteral("short"), first.side);
    EXPECT_LT(first.takeProfit, first.entryPrice);   // short TP below entry.
    EXPECT_GT(first.stopLoss, first.entryPrice);
}

TEST(EngineStrategyAdapterTest, SustainedDowntrendStaysFlat)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = alignedNow();
    feedTicks(adapter, 4400.0, 0.0, 12, now, 60'000);   // warm-up (bullish).
    feedTicks(adapter, 4390.0, -4.0, 20, now, 60'000);  // sustained down.
    const auto outcome = adapter.onTick(4310.0, now, 1.5, 1.0, 60'000);
    EXPECT_FALSE(outcome.decision.has_value());
}

TEST(EngineStrategyAdapterTest, TrendReadoutReflectsDirections)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = alignedNow();
    feedTicks(adapter, 4400.0, 0.0, 12, now, 60'000);
    const auto outcome = adapter.onTick(4400.0, now, 1.5, 1.0, 60'000);
    ASSERT_EQ(4, outcome.trends.size());
    // All timeframes report a valid line; directions are whatever the
    // indicator says — the values must be positive and ordered sane.
    for (const auto &trend : outcome.trends)
    {
        if (trend.valid)
        {
            EXPECT_GT(trend.supertrend, 0.0);
        }
    }
}

TEST(EngineStrategyAdapterTest, CooldownBlocksRepeatFlip)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = alignedNow();
    feedTicks(adapter, 4400.0, 0.0, 12, now, 60'000);
    feedTicks(adapter, 4398.0, -2.0, 8, now, 60'000);   // commit bearish.

    // Flip to bullish at tick T.
    std::int64_t triggerNow = -1;
    for (int i = 1; i <= 15; ++i)
    {
        const auto outcome = adapter.onTick(4382.0 + 3.0 * i, now, 5.0, 5.0, 60'000);
        if (outcome.decision.has_value() && outcome.decision->shouldOpen)
        {
            triggerNow = now;
            break;
        }
        now += 60'000;
    }
    ASSERT_GT(triggerNow, 0);

    // Cooldown 60s: evaluate immediately after — no new signal.
    const auto after = adapter.onTick(4432.0, triggerNow + 10'000, 5.0, 5.0, 60'000);
    if (after.decision.has_value())
    {
        EXPECT_FALSE(after.decision->shouldOpen);
        EXPECT_TRUE(after.decision->reason.contains(QStringLiteral("冷却")));
    }
}

// ---------------------------------------------------------------------------
// parseKline — Gate TradFi klines use the compact {o,h,l,c,t} schema
// ---------------------------------------------------------------------------

TEST(EngineStrategyAdapterTest, ParseKlineCompactSchema)
{
    // Real Gate TradFi kline: t is Unix SECONDS.
    const nlohmann::json obj = {
        {"o", "4400.50"}, {"h", "4405.00"}, {"l", "4399.10"}, {"c", "4403.20"},
        {"t", 1700000040},
    };
    pulse::market::Kline out;
    ASSERT_TRUE(EngineStrategyAdapter::parseKline(obj, out));
    EXPECT_DOUBLE_EQ(4400.5, out.open);
    EXPECT_DOUBLE_EQ(4405.0, out.high);
    EXPECT_DOUBLE_EQ(4399.1, out.low);
    EXPECT_DOUBLE_EQ(4403.2, out.close);
    EXPECT_EQ(1700000040LL * 1000, out.open_time);  // seconds → ms
    EXPECT_TRUE(out.closed);
}

TEST(EngineStrategyAdapterTest, ParseKlineFullNameFallback)
{
    // Other Gate products: full names, timestamp already in ms.
    const nlohmann::json obj = {
        {"open", "1.5"}, {"high", "2.0"}, {"low", "1.0"}, {"close", "1.8"},
        {"timestamp", 1700000040000LL},
    };
    pulse::market::Kline out;
    ASSERT_TRUE(EngineStrategyAdapter::parseKline(obj, out));
    EXPECT_DOUBLE_EQ(1.8, out.close);
    EXPECT_EQ(1700000040000LL, out.open_time);
}

TEST(EngineStrategyAdapterTest, ParseKlineMissingFieldsRejected)
{
    pulse::market::Kline out;
    EXPECT_FALSE(EngineStrategyAdapter::parseKline(nlohmann::json{{"o", "1.0"}}, out));
    EXPECT_FALSE(EngineStrategyAdapter::parseKline(nlohmann::json(nullptr), out));
}
