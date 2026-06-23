// test_strategy_base.cpp — Unit tests for StrategyBase (Layer 6 Strategy Engine)

#include "strategy/StrategyBase.hpp"

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
        return m_params;
    }

    void onTick(const market::Ticker & /*ticker*/) override
    {
        ++tick_count_;
    }

    void onKline(const market::Kline & /*kline*/) override
    {
        ++m_klineCount;
    }

    void onOrderbook(const market::OrderBook & /*book*/) override
    {
        ++m_orderbookCount;
    }

    // Expose protected emitSignal for testing.
    void test_emit_signal(const TradingSignal &signal)
    {
        emitSignal(signal);
    }

    StrategyParams m_params;
    int tick_count_ = 0;
    int m_klineCount = 0;
    int m_orderbookCount = 0;
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

    strategy.onTick(ticker);
    strategy.onTick(ticker);
    strategy.onKline(kline);
    strategy.onOrderbook(book);

    EXPECT_EQ(2, strategy.tick_count_);
    EXPECT_EQ(1, strategy.m_klineCount);
    EXPECT_EQ(1, strategy.m_orderbookCount);
}

// ---------------------------------------------------------------------------
// Signal emission
// ---------------------------------------------------------------------------

TEST(StrategyBase, EmitSignalWithCallback)
{
    MockStrategy strategy;
    strategy.m_params.min_confidence.store(0.5, std::memory_order_release);

    std::vector<TradingSignal> received;
    strategy.setSignalCallback([&](const TradingSignal &s)
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
    strategy.m_params.min_confidence.store(0.5, std::memory_order_release);

    std::vector<TradingSignal> received;
    strategy.setSignalCallback([&](const TradingSignal &s)
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
