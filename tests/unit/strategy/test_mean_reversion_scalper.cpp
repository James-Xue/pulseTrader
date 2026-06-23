// test_mean_reversion_scalper.cpp — Unit tests for Bollinger Band strategy (Layer 6 Strategy Engine)

#include "strategy/scalping/MeanReversionScalper.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace pulse;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// Helper: create a MeanReversionScalper
// ---------------------------------------------------------------------------
static std::unique_ptr<MeanReversionScalper> make_scalper(double bb_period = 20.0,
    double bb_std = 2.0)
{
    StrategyContext ctx;
    ctx.config.name = "mean_reversion_scalper";
    ctx.config.symbol = "ETH_USDT";
    ctx.config.order_quantity = 0.01;
    ctx.config.min_confidence = 0.0;
    ctx.config.poll_interval_ms = 100;

    auto scalper = std::make_unique<MeanReversionScalper>(ctx);
    scalper->params().bb_period.store(bb_period, std::memory_order_release);
    scalper->params().bb_std_dev.store(bb_std, std::memory_order_release);
    scalper->params().cooldown_seconds.store(0.0, std::memory_order_release);
    return scalper;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(MeanReversionScalper, NameAndId)
{
    auto scalper = make_scalper();
    EXPECT_EQ("MeanReversionScalper", scalper->name());
    EXPECT_EQ("mean_reversion_scalper_ETH_USDT", scalper->id());
}

TEST(MeanReversionScalper, DefaultParams)
{
    auto scalper = make_scalper();
    EXPECT_DOUBLE_EQ(20.0, scalper->params().bb_period.load());
    EXPECT_DOUBLE_EQ(2.0, scalper->params().bb_std_dev.load());
}

TEST(MeanReversionScalper, OnTickIgnored)
{
    auto scalper = make_scalper();
    market::Ticker ticker;
    scalper->onTick(ticker);
}

TEST(MeanReversionScalper, OnOrderbookIgnored)
{
    auto scalper = make_scalper();
    market::OrderBook book;
    scalper->onOrderbook(book);
}

TEST(MeanReversionScalper, InsufficientDataNoSignal)
{
    auto scalper = make_scalper(20.0, 2.0);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // onKline without market feed — should return early.
    market::Kline kline;
    kline.closed = true;
    scalper->onKline(kline);

    EXPECT_TRUE(received.empty());
}

TEST(MeanReversionScalper, ParamsHotReload)
{
    auto scalper = make_scalper();

    // Simulate AI layer updating params.
    scalper->params().bb_period.store(30.0, std::memory_order_release);
    scalper->params().bb_std_dev.store(1.5, std::memory_order_release);

    EXPECT_DOUBLE_EQ(30.0, scalper->params().bb_period.load(std::memory_order_acquire));
    EXPECT_DOUBLE_EQ(1.5, scalper->params().bb_std_dev.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// Bollinger Band signal generation
//
// Note: Full BB testing requires a MarketFeed with KlineBuffer populated.
// The smoke test tool (tools/test_strategy.cpp) exercises the full pipeline.
// Here we verify interface correctness and parameter handling.
// ---------------------------------------------------------------------------

TEST(MeanReversionScalper, ConfidenceRange)
{
    auto scalper = make_scalper();
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    EXPECT_GE(scalper->params().min_confidence.load(), 0.0);
    EXPECT_LE(scalper->params().min_confidence.load(), 1.0);
}
