// test_gold_strategy.cpp — unit tests for the v1 gold strategy engine.
//
// The engine is pure logic (prices in, decision out) — no network involved.

#include "GoldStrategyEngine.hpp"

#include <gtest/gtest.h>

using namespace pulse::ui;

namespace
{

// Feed a monotonically rising price series (long-friendly regime).
void feedRising(GoldStrategyEngine &engine, double start, double step, int n,
                std::int64_t &nowMs)
{
    for (int i = 0; i < n; ++i)
    {
        engine.onPrice(start + step * i, nowMs);
        nowMs += 1000;
    }
}

// Feed a monotonically falling series.
void feedFalling(GoldStrategyEngine &engine, double start, double step, int n,
                 std::int64_t &nowMs)
{
    for (int i = 0; i < n; ++i)
    {
        engine.onPrice(start - step * i, nowMs);
        nowMs += 1000;
    }
}

} // namespace

TEST(GoldStrategyTest, WarmupRequiresHistory)
{
    // EMA30 + volWindow20 = 50 ticks needed (size < 50 blocks, size == 50
    // passes); early ticks must not open.
    GoldStrategyEngine engine;
    std::int64_t now = 1'700'000'000'000LL;
    const GoldDecision early = engine.onPrice(4400.0, now);  // tick 1
    EXPECT_FALSE(early.shouldOpen);
    EXPECT_TRUE(early.reason.contains(QStringLiteral("预热")));

    feedRising(engine, 4400.5, 0.5, 47, now);                // ticks 2..48 → size 48
    EXPECT_FALSE(engine.onPrice(4423.5, now).shouldOpen);    // tick 49 < 50 — still warming
    const GoldDecision ready = engine.onPrice(4424.0, now);  // tick 50 — warm-up done
    EXPECT_TRUE(ready.shouldOpen) << "reason=" << ready.reason.toStdString();
}

TEST(GoldStrategyTest, RisingTrendOpensWithEnvelope)
{
    // Warm-up completes exactly at 50 ticks — feed 49, then evaluate the
    // 50th so the cooldown (60 s) cannot interfere.
    GoldStrategyEngine engine;
    std::int64_t now = 1'700'000'000'000LL;
    feedRising(engine, 4400.0, 0.5, 49, now);
    const GoldDecision decision = engine.onPrice(4424.5, now);
    ASSERT_TRUE(decision.shouldOpen) << "reason=" << decision.reason.toStdString();
    EXPECT_DOUBLE_EQ(4424.5, decision.entryPrice);
    // TP above entry, SL below entry, both on the correct side.
    EXPECT_GT(decision.takeProfit, decision.entryPrice);
    EXPECT_LT(decision.stopLoss, decision.entryPrice);
}

TEST(GoldStrategyTest, FallingTrendStaysFlat)
{
    GoldStrategyEngine engine;
    std::int64_t now = 1'700'000'000'000LL;
    feedFalling(engine, 4500.0, 0.5, 60, now);
    const GoldDecision decision = engine.onPrice(4470.0, now);
    EXPECT_FALSE(decision.shouldOpen);
    EXPECT_TRUE(decision.reason.contains(QStringLiteral("趋势偏空")));
}

TEST(GoldStrategyTest, CooldownSuppressesRepeatSignals)
{
    GoldStrategyEngine engine;
    std::int64_t now = 1'700'000'000'000LL;
    feedRising(engine, 4400.0, 0.5, 49, now);

    EXPECT_TRUE(engine.onPrice(4424.5, now).shouldOpen);       // signal 1 (tick 50)
    now += 10'000;                                             // 10 s later
    const GoldDecision second = engine.onPrice(4426.0, now);
    EXPECT_FALSE(second.shouldOpen);                           // cooldown blocks
    EXPECT_TRUE(second.reason.contains(QStringLiteral("冷却")));

    now += 60'000;                                             // past the cooldown
    EXPECT_TRUE(engine.onPrice(4428.0, now).shouldOpen);       // signal 2
}

TEST(GoldStrategyTest, DeadMarketVolatilityFloor)
{
    // Flat prices → zero volatility → below the floor → no signal.
    GoldStrategyEngine engine;
    std::int64_t now = 1'700'000'000'000LL;
    for (int i = 0; i < 60; ++i)
    {
        engine.onPrice(4400.0, now);
        now += 1000;
    }
    const GoldDecision decision = engine.onPrice(4400.0, now);
    EXPECT_FALSE(decision.shouldOpen);
    EXPECT_TRUE(decision.reason.contains(QStringLiteral("波幅过低")));
}

TEST(GoldStrategyTest, SpikeVolatilityCeiling)
{
    // A 100 USD jump inside the window exceeds the 50 USD ceiling.
    GoldStrategyEngine engine;
    std::int64_t now = 1'700'000'000'000LL;
    feedRising(engine, 4400.0, 0.1, 50, now);
    const GoldDecision decision = engine.onPrice(4500.0, now);
    EXPECT_FALSE(decision.shouldOpen);
    EXPECT_TRUE(decision.reason.contains(QStringLiteral("波幅过大")));
}

TEST(GoldStrategyTest, ResetClearsHistory)
{
    GoldStrategyEngine engine;
    std::int64_t now = 1'700'000'000'000LL;
    feedRising(engine, 4400.0, 0.5, 60, now);
    EXPECT_EQ(60, engine.priceCount());
    engine.reset();
    EXPECT_EQ(0, engine.priceCount());
    EXPECT_FALSE(engine.onPrice(4430.0, now).shouldOpen);  // back to warm-up
}
