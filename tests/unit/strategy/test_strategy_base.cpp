// test_strategy_base.cpp — Unit tests for StrategyBase (Layer 6 Strategy Engine)

#include "strategy/strategy_base.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace pulse;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// MockStrategy — concrete implementation for testing StrategyBase
// ---------------------------------------------------------------------------
class MockStrategy : public StrategyBase
{
  public:
    [[nodiscard]] std::string name() const override
    {
        return "MockStrategy";
    }

    [[nodiscard]] std::string id() const override
    {
        return "mock_strategy_TEST";
    }

    [[nodiscard]] StrategyParams &params() override
    {
        return params_;
    }

    void on_tick(const market::Ticker & /*ticker*/) override
    {
        ++tick_count_;
    }

    void on_kline(const market::Kline & /*kline*/) override
    {
        ++kline_count_;
    }

    void on_orderbook(const market::OrderBook & /*book*/) override
    {
        ++orderbook_count_;
    }

    // Expose protected emit_signal for testing.
    void test_emit_signal(const TradingSignal &signal)
    {
        emit_signal(signal);
    }

    StrategyParams params_;
    int tick_count_ = 0;
    int kline_count_ = 0;
    int orderbook_count_ = 0;
};

// ---------------------------------------------------------------------------
// Lifecycle hooks
// ---------------------------------------------------------------------------

TEST(StrategyBase, NameAndId)
{
    MockStrategy strategy;
    EXPECT_EQ("MockStrategy", strategy.name());
    EXPECT_EQ("mock_strategy_TEST", strategy.id());
}

TEST(StrategyBase, ActiveFlag)
{
    MockStrategy strategy;

    // Default is inactive.
    EXPECT_FALSE(strategy.active().load());

    // Activate.
    strategy.active().store(true, std::memory_order_release);
    EXPECT_TRUE(strategy.active().load(std::memory_order_acquire));
}

TEST(StrategyBase, LifecycleHookCounters)
{
    MockStrategy strategy;

    market::Ticker ticker;
    market::Kline kline;
    market::OrderBook book;

    strategy.on_tick(ticker);
    strategy.on_tick(ticker);
    strategy.on_kline(kline);
    strategy.on_orderbook(book);

    EXPECT_EQ(2, strategy.tick_count_);
    EXPECT_EQ(1, strategy.kline_count_);
    EXPECT_EQ(1, strategy.orderbook_count_);
}

// ---------------------------------------------------------------------------
// Signal emission
// ---------------------------------------------------------------------------

TEST(StrategyBase, EmitSignalWithCallback)
{
    MockStrategy strategy;
    strategy.params_.min_confidence.store(0.5, std::memory_order_release);

    std::vector<TradingSignal> received;
    strategy.set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // Signal above threshold — should be emitted.
    TradingSignal signal;
    signal.type = SignalType::Buy;
    signal.symbol = "BTC_USDT";
    signal.confidence = 0.8;
    signal.price = 50000.0;
    signal.strategy_id = "mock_strategy_TEST";
    signal.reason = "Test signal";

    strategy.test_emit_signal(signal);

    ASSERT_EQ(1u, received.size());
    EXPECT_EQ(SignalType::Buy, received[0].type);
    EXPECT_EQ("BTC_USDT", received[0].symbol);
    EXPECT_DOUBLE_EQ(0.8, received[0].confidence);
}

TEST(StrategyBase, EmitSignalBelowThresholdDropped)
{
    MockStrategy strategy;
    strategy.params_.min_confidence.store(0.5, std::memory_order_release);

    std::vector<TradingSignal> received;
    strategy.set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // Signal below threshold — should be dropped.
    TradingSignal signal;
    signal.type = SignalType::Sell;
    signal.symbol = "ETH_USDT";
    signal.confidence = 0.3; // Below 0.5 threshold.

    strategy.test_emit_signal(signal);

    EXPECT_TRUE(received.empty());
}

TEST(StrategyBase, EmitSignalWithoutCallback)
{
    MockStrategy strategy;

    // No callback set — should not crash.
    TradingSignal signal;
    signal.type = SignalType::Buy;
    signal.confidence = 0.9;

    // Should silently drop (no callback).
    strategy.test_emit_signal(signal);
}
