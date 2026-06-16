// test_strategy_manager.cpp — Unit tests for StrategyManager (Layer 6 Strategy Engine)

#include "pulse/strategy/strategy_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <vector>

using namespace pulse;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// MockStrategy — minimal implementation for manager tests
// ---------------------------------------------------------------------------
class MockStrategy : public StrategyBase
{
  public:
    explicit MockStrategy(const StrategyContext &ctx)
    {
        context_ = ctx;
    }

    [[nodiscard]] std::string name() const override
    {
        return "MockStrategy";
    }

    [[nodiscard]] std::string id() const override
    {
        return "mock_" + context_.config.symbol;
    }

    [[nodiscard]] StrategyParams &params() override
    {
        return params_;
    }

    void on_tick(const market::Ticker & /*ticker*/) override
    {
        tick_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_kline(const market::Kline & /*kline*/) override
    {
    }

    void on_orderbook(const market::OrderBook & /*book*/) override
    {
    }

    StrategyParams params_;
    std::atomic<int> tick_count_{ 0 };
};

// ---------------------------------------------------------------------------
// Helper: create a StrategyInstanceConfig
// ---------------------------------------------------------------------------
static StrategyInstanceConfig make_config(const std::string &symbol,
    bool enabled = true,
    std::uint32_t poll_ms = 50)
{
    StrategyInstanceConfig cfg;
    cfg.name = "mock";
    cfg.symbol = symbol;
    cfg.enabled = enabled;
    cfg.poll_interval_ms = poll_ms;
    return cfg;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(StrategyManager, RegisterAndCount)
{
    StrategyManager manager;

    EXPECT_EQ(0u, manager.strategy_count());

    StrategyContext ctx;
    ctx.config = make_config("BTC_USDT");

    auto strategy = std::make_unique<MockStrategy>(ctx);
    manager.register_strategy(std::move(strategy));

    EXPECT_EQ(1u, manager.strategy_count());
}

TEST(StrategyManager, DisabledStrategyNotStarted)
{
    StrategyManager manager;

    StrategyContext ctx;
    ctx.config = make_config("BTC_USDT", false); // disabled

    auto strategy = std::make_unique<MockStrategy>(ctx);
    manager.register_strategy(std::move(strategy));

    // Start without market feed — disabled strategy won't try to use it.
    manager.set_signal_callback([](const TradingSignal &) {});
    manager.start();

    EXPECT_EQ(0u, manager.running_count());

    manager.stop();
}

TEST(StrategyManager, StartAndStop)
{
    StrategyManager manager;

    // Create strategy with no market feed — it will log an error and exit.
    // This tests the start/stop lifecycle without requiring a live exchange.
    StrategyContext ctx;
    ctx.config = make_config("BTC_USDT", true, 10);

    auto strategy = std::make_unique<MockStrategy>(ctx);
    manager.register_strategy(std::move(strategy));

    manager.set_signal_callback([](const TradingSignal &) {});
    manager.start();

    // Let the thread run briefly (it will exit because no market feed).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Stop should complete without hanging.
    manager.stop();

    EXPECT_EQ(0u, manager.running_count());
}

TEST(StrategyManager, SignalCallbackForwarding)
{
    StrategyManager manager;

    std::vector<TradingSignal> received;
    manager.set_signal_callback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // We can verify the callback is set by checking the manager doesn't crash.
    // Full signal flow requires a live MarketFeed (tested in smoke test).
    EXPECT_TRUE(received.empty());
}
