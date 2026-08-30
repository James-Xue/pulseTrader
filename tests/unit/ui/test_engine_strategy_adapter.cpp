// test_engine_strategy_adapter.cpp — unit tests for the engine-strategy
// adapter (tick → 1m candles → EmaResonanceScalper evaluateEntry).
//
// No network: warmup() is not exercised here (it needs a REST client);
// the kline aggregation and decision logic are pure.

#include "EngineStrategyAdapter.hpp"

#include <gtest/gtest.h>

using namespace pulse::ui;

namespace
{

// Feed `n` ticks spaced exactly one minute apart (each tick lands in a new
// candle bucket) — mimics a fresh candle per poll.
void feedMinutely(EngineStrategyAdapter &adapter, double start, double step,
                  int n, std::int64_t &nowMs)
{
    for (int i = 0; i < n; ++i)
    {
        adapter.onTick(start + step * i, nowMs, 1.5, 1.0, 60'000);
        nowMs += 60'000;
    }
}

} // namespace

TEST(EngineStrategyAdapterTest, KlineAggregationBucketsTicksByMinute)
{
    EngineStrategyAdapter adapter;
    // Aligned to a minute boundary (divisible by 60'000) so +30s/+59s stay
    // inside the same bucket and +60s crosses into the next one.
    std::int64_t now = 1'700'000'040'000LL;
    adapter.onTick(4400.0, now, 1.5, 1.0, 60'000);
    adapter.onTick(4405.0, now + 30'000, 1.5, 1.0, 60'000);   // same bucket
    adapter.onTick(4390.0, now + 59'000, 1.5, 1.0, 60'000);   // same bucket
    EXPECT_EQ(1, adapter.klineCount());

    adapter.onTick(4410.0, now + 60'000, 1.5, 1.0, 60'000);   // new bucket
    EXPECT_EQ(2, adapter.klineCount());
}

TEST(EngineStrategyAdapterTest, WarmupRequiresEnoughCandles)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = 1'700'000'000'000LL;
    feedMinutely(adapter, 4400.0, 0.0, 50, now);   // 50 candles — below 201.
    const auto decision = adapter.onTick(4400.0, now, 1.5, 1.0, 60'000);
    ASSERT_TRUE(decision.has_value());
    EXPECT_FALSE(decision->shouldOpen);
    EXPECT_TRUE(decision->reason.contains(QStringLiteral("预热")));
    EXPECT_FALSE(adapter.suggestStops(4400.0, 1.5, 1.0).has_value());
}

TEST(EngineStrategyAdapterTest, RisingTrendTriggersBuyWithEnvelope)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = 1'700'000'040'000LL;

    // 1. Flat regime while warming up: the first evaluation (at 201 candles)
    //    only COMMITS the None state — no signal (strategy design: entries
    //    fire on the regime TRANSITION, not on the alignment itself).
    feedMinutely(adapter, 4400.0, 0.0, 200, now);
    EXPECT_FALSE(adapter.onTick(4400.0, now, 1.5, 1.0, 60'000)->shouldOpen);

    // 2. Strong uptrend: the EMA stack flips into a full bullish alignment —
    //    the None → Bull transition fires exactly one Buy signal.
    bool opened = false;
    pulse::ui::AdapterDecision first;
    for (int i = 1; i <= 60; ++i)
    {
        const auto d = adapter.onTick(4400.0 + 10.0 * i, now, 1.5, 1.0, 60'000);
        now += 60'000;
        if (d.has_value() && d->shouldOpen)
        {
            opened = true;
            first = *d;
            break;
        }
    }
    ASSERT_TRUE(opened) << "no Buy fired during the uptrend";
    EXPECT_GT(first.takeProfit, first.entryPrice);
    EXPECT_LT(first.stopLoss, first.entryPrice);
    EXPECT_GT(first.confidence, 0.0);
}

TEST(EngineStrategyAdapterTest, FallingTrendStaysFlat)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = 1'700'000'000'000LL;
    feedMinutely(adapter, 4500.0, -0.5, 240, now);   // downtrend → no Buy.
    const auto decision = adapter.onTick(4380.0, now, 1.5, 1.0, 60'000);
    ASSERT_TRUE(decision.has_value());
    EXPECT_FALSE(decision->shouldOpen);
}

TEST(EngineStrategyAdapterTest, CooldownGatesRepeatBuys)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = 1'700'000'040'000LL;
    feedMinutely(adapter, 4400.0, 0.0, 200, now);
    EXPECT_FALSE(adapter.onTick(4400.0, now, 1.5, 1.0, 60'000)->shouldOpen);

    // Fire the transition Buy; record the signal timestamp.
    std::int64_t triggerNow = -1;
    for (int i = 1; i <= 60; ++i)
    {
        const auto d = adapter.onTick(4400.0 + 10.0 * i, now, 1.5, 1.0, 60'000);
        if (d.has_value() && d->shouldOpen)
        {
            triggerNow = now;
            break;
        }
        now += 60'000;
    }
    ASSERT_GT(triggerNow, 0);

    // 10 s later, same alignment — cooldown (60 s) blocks the repeat.
    const auto second = adapter.onTick(4600.0, triggerNow + 10'000, 1.5, 1.0, 60'000);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->shouldOpen);
    EXPECT_TRUE(second->reason.contains(QStringLiteral("冷却")));
}

TEST(EngineStrategyAdapterTest, SuggestStopsMirrorsEnvelope)
{
    EngineStrategyAdapter adapter;
    std::int64_t now = 1'700'000'000'000LL;
    feedMinutely(adapter, 4400.0, 0.5, 240, now);
    const auto stops = adapter.suggestStops(4520.0, 1.5, 1.0);
    ASSERT_TRUE(stops.has_value());
    EXPECT_GT(stops->takeProfit, 4520.0);
    EXPECT_LT(stops->stopLoss, 4520.0);
    EXPECT_GT(stops->takeProfit, stops->stopLoss);
}
