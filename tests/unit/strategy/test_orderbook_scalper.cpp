// test_orderbook_scalper.cpp — Unit tests for order book imbalance strategy (Layer 6 Strategy Engine)

#include "pulse/strategy/scalping/orderbook_scalper.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace pulse;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// Helper: create an OrderBookScalper
// ---------------------------------------------------------------------------
static std::unique_ptr<OrderBookScalper> make_scalper(double threshold = 0.3,
    double cooldown = 0.0)
{
    StrategyContext ctx;
    ctx.config.name = "orderbook_scalper";
    ctx.config.symbol = "BTC_USDT";
    ctx.config.order_quantity = 0.001;
    ctx.config.min_confidence = 0.0;
    ctx.config.poll_interval_ms = 100;

    auto scalper = std::make_unique<OrderBookScalper>(ctx);
    scalper->params().ob_imbalance_threshold.store(threshold, std::memory_order_release);
    scalper->params().ob_depth.store(5.0, std::memory_order_release);
    scalper->params().cooldown_seconds.store(cooldown, std::memory_order_release);
    return scalper;
}

// ---------------------------------------------------------------------------
// Helper: build an OrderBook with specified bid/ask volumes
// ---------------------------------------------------------------------------
static market::OrderBook make_book(double bid_vol, double ask_vol, int levels = 5)
{
    market::OrderBook book;
    book.symbol = "BTC_USDT";
    book.timestamp = 1000;

    // Bids: descending from 50000.
    for (int i = 0; i < levels; ++i)
    {
        double price = 50000.0 - i * 1.0;
        book.bids[price] = bid_vol / levels;
    }

    // Asks: ascending from 50001.
    for (int i = 0; i < levels; ++i)
    {
        double price = 50001.0 + i * 1.0;
        book.asks[price] = ask_vol / levels;
    }

    return book;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(OrderBookScalper, NameAndId)
{
    auto scalper = make_scalper();
    EXPECT_EQ("OrderBookScalper", scalper->name());
    EXPECT_EQ("orderbook_scalper_BTC_USDT", scalper->id());
}

TEST(OrderBookScalper, OnTickIgnored)
{
    auto scalper = make_scalper();
    market::Ticker ticker;
    scalper->on_tick(ticker);
}

TEST(OrderBookScalper, OnKlineIgnored)
{
    auto scalper = make_scalper();
    market::Kline kline;
    scalper->on_kline(kline);
}

TEST(OrderBookScalper, BuySignalOnBidDominance)
{
    auto scalper = make_scalper(0.3, 0.0);

    std::vector<TradingSignal> received;
    scalper->set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // Bid volume = 10, Ask volume = 2 → imbalance = (10-2)/(10+2) = 0.667 > 0.3
    auto book = make_book(10.0, 2.0);
    scalper->on_orderbook(book);

    ASSERT_EQ(1u, received.size());
    EXPECT_EQ(SignalType::Buy, received[0].type);
    EXPECT_GT(received[0].confidence, 0.3);
    EXPECT_EQ("BTC_USDT", received[0].symbol);
}

TEST(OrderBookScalper, SellSignalOnAskDominance)
{
    auto scalper = make_scalper(0.3, 0.0);

    std::vector<TradingSignal> received;
    scalper->set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // Bid volume = 2, Ask volume = 10 → imbalance = (2-10)/(2+10) = -0.667 < -0.3
    auto book = make_book(2.0, 10.0);
    scalper->on_orderbook(book);

    ASSERT_EQ(1u, received.size());
    EXPECT_EQ(SignalType::Sell, received[0].type);
    EXPECT_GT(received[0].confidence, 0.3);
}

TEST(OrderBookScalper, NoSignalOnBalancedBook)
{
    auto scalper = make_scalper(0.3, 0.0);

    std::vector<TradingSignal> received;
    scalper->set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // Balanced: imbalance = 0 → no signal.
    auto book = make_book(5.0, 5.0);
    scalper->on_orderbook(book);

    EXPECT_TRUE(received.empty());
}

TEST(OrderBookScalper, InsufficientDepth)
{
    auto scalper = make_scalper(0.3, 0.0);

    std::vector<TradingSignal> received;
    scalper->set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // Only 2 levels but ob_depth = 5 → skip.
    auto book = make_book(10.0, 2.0, 2);
    scalper->on_orderbook(book);

    EXPECT_TRUE(received.empty());
}

TEST(OrderBookScalper, CooldownPreventsDuplicate)
{
    auto scalper = make_scalper(0.3, 60.0); // 60 second cooldown.

    std::vector<TradingSignal> received;
    scalper->set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    auto book = make_book(10.0, 2.0);

    // First call — should emit.
    scalper->on_orderbook(book);
    EXPECT_EQ(1u, received.size());

    // Second call immediately after — should be blocked by cooldown.
    scalper->on_orderbook(book);
    EXPECT_EQ(1u, received.size()); // Still 1.
}

TEST(OrderBookScalper, ConfidenceClamped)
{
    auto scalper = make_scalper(0.01, 0.0);

    std::vector<TradingSignal> received;
    scalper->set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // Extreme imbalance: bid=100, ask=0.01 → imbalance ≈ 0.9999
    auto book = make_book(100.0, 0.01);
    scalper->on_orderbook(book);

    ASSERT_EQ(1u, received.size());
    EXPECT_GE(received[0].confidence, 0.0);
    EXPECT_LE(received[0].confidence, 1.0);
}
