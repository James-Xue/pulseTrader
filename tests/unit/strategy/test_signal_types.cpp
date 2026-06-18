// test_signal_types.cpp — Unit tests for signal type definitions (Layer 6 Strategy Engine)

#include "strategy/signal_types.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// SignalType enum
// ---------------------------------------------------------------------------

TEST(SignalType, EnumValues)
{
    // Verify the three signal types exist and are distinct.
    EXPECT_NE(SignalType::Buy, SignalType::Sell);
    EXPECT_NE(SignalType::Buy, SignalType::Flat);
    EXPECT_NE(SignalType::Sell, SignalType::Flat);
}

// ---------------------------------------------------------------------------
// TradingSignal defaults
// ---------------------------------------------------------------------------

TEST(TradingSignal, DefaultConstructor)
{
    TradingSignal signal;

    EXPECT_EQ(SignalType::Flat, signal.type);
    EXPECT_TRUE(signal.symbol.empty());
    EXPECT_DOUBLE_EQ(0.0, signal.confidence);
    EXPECT_DOUBLE_EQ(0.0, signal.price);
    EXPECT_TRUE(signal.strategy_id.empty());
    EXPECT_TRUE(signal.reason.empty());
}

TEST(TradingSignal, ManualConstruction)
{
    TradingSignal signal;
    signal.type = SignalType::Buy;
    signal.symbol = "BTC_USDT";
    signal.confidence = 0.85;
    signal.price = 50000.0;
    signal.strategy_id = "momentum_scalper_BTC_USDT";
    signal.timestamp = now();
    signal.reason = "EMA bullish crossover";

    EXPECT_EQ(SignalType::Buy, signal.type);
    EXPECT_EQ("BTC_USDT", signal.symbol);
    EXPECT_DOUBLE_EQ(0.85, signal.confidence);
    EXPECT_DOUBLE_EQ(50000.0, signal.price);
    EXPECT_EQ("momentum_scalper_BTC_USDT", signal.strategy_id);
    EXPECT_EQ("EMA bullish crossover", signal.reason);
}

TEST(TradingSignal, CopyConstruction)
{
    TradingSignal original;
    original.type = SignalType::Sell;
    original.symbol = "ETH_USDT";
    original.confidence = 0.72;
    original.price = 3000.0;
    original.strategy_id = "orderbook_scalper_ETH_USDT";
    original.reason = "Order book sell pressure";

    TradingSignal copy = original;

    EXPECT_EQ(original.type, copy.type);
    EXPECT_EQ(original.symbol, copy.symbol);
    EXPECT_DOUBLE_EQ(original.confidence, copy.confidence);
    EXPECT_DOUBLE_EQ(original.price, copy.price);
    EXPECT_EQ(original.strategy_id, copy.strategy_id);
    EXPECT_EQ(original.reason, copy.reason);
}
