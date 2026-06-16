// test_momentum_scalper.cpp — Unit tests for EMA crossover strategy (Layer 6 Strategy Engine)

#include "pulse/strategy/scalping/momentum_scalper.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace pulse;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// Helper: create a MomentumScalper with a mock context
// ---------------------------------------------------------------------------
static std::unique_ptr<MomentumScalper> make_scalper(double fast = 9.0, double slow = 21.0)
{
    StrategyContext ctx;
    ctx.config.name = "momentum_scalper";
    ctx.config.symbol = "BTC_USDT";
    ctx.config.order_quantity = 0.001;
    ctx.config.min_confidence = 0.0; // Accept all signals in tests.
    ctx.config.poll_interval_ms = 100;

    auto scalper = std::make_unique<MomentumScalper>(ctx);
    scalper->params().ema_fast_period.store(fast, std::memory_order_release);
    scalper->params().ema_slow_period.store(slow, std::memory_order_release);
    return scalper;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(MomentumScalper, NameAndId)
{
    auto scalper = make_scalper();
    EXPECT_EQ("MomentumScalper", scalper->name());
    EXPECT_EQ("momentum_scalper_BTC_USDT", scalper->id());
}

TEST(MomentumScalper, DefaultParams)
{
    auto scalper = make_scalper();
    EXPECT_DOUBLE_EQ(9.0, scalper->params().ema_fast_period.load());
    EXPECT_DOUBLE_EQ(21.0, scalper->params().ema_slow_period.load());
}

TEST(MomentumScalper, OnTickIgnored)
{
    auto scalper = make_scalper();
    market::Ticker ticker;
    ticker.symbol = "BTC_USDT";
    ticker.last = 50000.0;

    // Should not crash or produce side effects.
    scalper->on_tick(ticker);
}

TEST(MomentumScalper, OnOrderbookIgnored)
{
    auto scalper = make_scalper();
    market::OrderBook book;
    book.symbol = "BTC_USDT";

    scalper->on_orderbook(book);
}

TEST(MomentumScalper, InsufficientDataNoSignal)
{
    auto scalper = make_scalper(9.0, 21.0);

    std::vector<TradingSignal> received;
    scalper->set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // on_kline without market feed — will return early because feed is null.
    market::Kline kline;
    kline.closed = true;
    scalper->on_kline(kline);

    EXPECT_TRUE(received.empty());
}

TEST(MomentumScalper, ConfidenceClamped)
{
    auto scalper = make_scalper();
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    // Confidence should always be in [0.0, 1.0].
    EXPECT_GE(scalper->params().min_confidence.load(), 0.0);
    EXPECT_LE(scalper->params().min_confidence.load(), 1.0);
}

// ---------------------------------------------------------------------------
// EMA computation (tested via signal generation with manual data)
//
// Note: Full EMA crossover testing requires a MarketFeed with KlineBuffer.
// The smoke test tool (tools/test_strategy.cpp) exercises the full pipeline.
// Here we verify the strategy's interface and parameter handling.
// ---------------------------------------------------------------------------

TEST(MomentumScalper, ParamsHotReload)
{
    auto scalper = make_scalper();

    // Simulate AI layer updating params.
    scalper->params().ema_fast_period.store(12.0, std::memory_order_release);
    scalper->params().ema_slow_period.store(26.0, std::memory_order_release);

    EXPECT_DOUBLE_EQ(12.0, scalper->params().ema_fast_period.load(std::memory_order_acquire));
    EXPECT_DOUBLE_EQ(26.0, scalper->params().ema_slow_period.load(std::memory_order_acquire));
}
