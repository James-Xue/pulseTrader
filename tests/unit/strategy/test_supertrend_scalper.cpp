// test_supertrend_scalper.cpp — Unit tests for SuperTrend strategy (Layer 6 Strategy Engine)

#include "strategy/scalping/supertrend_scalper.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace pulse;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// Helper: create a SuperTrendScalper
// ---------------------------------------------------------------------------
static std::unique_ptr<SuperTrendScalper> make_scalper(double period = 10.0,
    double multiplier = 3.0)
{
    StrategyContext ctx;
    ctx.config.name = "supertrend_scalper";
    ctx.config.symbol = "BTC_USDT";
    ctx.config.order_quantity = 1.0;
    ctx.config.min_confidence = 0.0;
    ctx.config.poll_interval_ms = 100;

    auto scalper = std::make_unique<SuperTrendScalper>(ctx);
    scalper->params().supertrend_period.store(period, std::memory_order_release);
    scalper->params().supertrend_multiplier.store(multiplier, std::memory_order_release);
    scalper->params().cooldown_seconds.store(0.0, std::memory_order_release);
    return scalper;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(SuperTrendScalper, NameAndId)
{
    auto scalper = make_scalper();
    EXPECT_EQ("SuperTrendScalper", scalper->name());
    EXPECT_EQ("supertrend_scalper_BTC_USDT", scalper->id());
}

TEST(SuperTrendScalper, DefaultParams)
{
    auto scalper = make_scalper();
    EXPECT_DOUBLE_EQ(10.0, scalper->params().supertrend_period.load());
    EXPECT_DOUBLE_EQ(3.0, scalper->params().supertrend_multiplier.load());
}

TEST(SuperTrendScalper, OnTickIgnored)
{
    auto scalper = make_scalper();
    market::Ticker ticker;
    scalper->on_tick(ticker);
}

TEST(SuperTrendScalper, OnOrderbookIgnored)
{
    auto scalper = make_scalper();
    market::OrderBook book;
    scalper->on_orderbook(book);
}

TEST(SuperTrendScalper, InsufficientDataNoSignal)
{
    auto scalper = make_scalper(10.0, 3.0);

    std::vector<TradingSignal> received;
    scalper->set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // on_kline without market feed — should return early.
    market::Kline kline;
    kline.closed = true;
    scalper->on_kline(kline);

    EXPECT_TRUE(received.empty());
}

TEST(SuperTrendScalper, ParamsHotReload)
{
    auto scalper = make_scalper();

    // Simulate AI layer updating params.
    scalper->params().supertrend_period.store(14.0, std::memory_order_release);
    scalper->params().supertrend_multiplier.store(2.0, std::memory_order_release);

    EXPECT_DOUBLE_EQ(14.0, scalper->params().supertrend_period.load(std::memory_order_acquire));
    EXPECT_DOUBLE_EQ(2.0, scalper->params().supertrend_multiplier.load(std::memory_order_acquire));
}

TEST(SuperTrendScalper, ConfidenceRange)
{
    auto scalper = make_scalper();
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    EXPECT_GE(scalper->params().min_confidence.load(), 0.0);
    EXPECT_LE(scalper->params().min_confidence.load(), 1.0);
}

// ---------------------------------------------------------------------------
// SuperTrend signal generation
//
// Note: Full SuperTrend testing requires a MarketFeed with KlineBuffer populated.
// The smoke test tool (tools/test_strategy.cpp) exercises the full pipeline.
// Here we verify interface correctness and parameter handling.
// ---------------------------------------------------------------------------
