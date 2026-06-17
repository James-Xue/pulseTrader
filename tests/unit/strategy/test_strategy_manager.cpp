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
//
// Wrapped in an anonymous namespace to avoid ODR collision with the
// MockStrategy defined in test_strategy_base.cpp (same executable).
// ---------------------------------------------------------------------------
namespace
{

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

} // anonymous namespace

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

// ---------------------------------------------------------------------------
// snapshot() — interface gap bridge for dashboard
// ---------------------------------------------------------------------------

TEST(StrategyManager, SnapshotEmptyManager)
{
    // An empty manager must return an empty snapshot vector.
    StrategyManager manager;
    const auto snaps = manager.snapshot();
    EXPECT_TRUE(snaps.empty());
}

TEST(StrategyManager, SnapshotContainsRegisteredStrategies)
{
    // After registering strategies, snapshot() must include each one with
    // correct name, id, symbol, enabled flag, and running state.
    StrategyManager manager;

    // Register an enabled strategy.
    {
        StrategyContext ctx;
        ctx.config = make_config("BTC_USDT", true, 100);
        auto strategy = std::make_unique<MockStrategy>(ctx);
        manager.register_strategy(std::move(strategy));
    }

    // Register a disabled strategy.
    {
        StrategyContext ctx;
        ctx.config = make_config("ETH_USDT", false, 200);
        auto strategy = std::make_unique<MockStrategy>(ctx);
        manager.register_strategy(std::move(strategy));
    }

    // Snapshot before start() — both present, neither running.
    const auto snaps = manager.snapshot();
    ASSERT_EQ(snaps.size(), 2u);

    // Find each snapshot by symbol (order matches registration order).
    const auto &btc_snap = snaps[0];
    EXPECT_EQ(btc_snap.name, "MockStrategy");
    EXPECT_EQ(btc_snap.id, "mock_BTC_USDT");
    EXPECT_EQ(btc_snap.symbol, "BTC_USDT");
    EXPECT_TRUE(btc_snap.enabled);
    EXPECT_FALSE(btc_snap.running);
    EXPECT_EQ(btc_snap.poll_interval_ms, 100u);

    const auto &eth_snap = snaps[1];
    EXPECT_EQ(eth_snap.name, "MockStrategy");
    EXPECT_EQ(eth_snap.id, "mock_ETH_USDT");
    EXPECT_EQ(eth_snap.symbol, "ETH_USDT");
    EXPECT_FALSE(eth_snap.enabled);
    EXPECT_FALSE(eth_snap.running);
    EXPECT_EQ(eth_snap.poll_interval_ms, 200u);
}

TEST(StrategyManager, SnapshotRunningStateAfterStart)
{
    // After start(), enabled strategies should briefly show running = true.
    StrategyManager manager;

    StrategyContext ctx;
    ctx.config = make_config("BTC_USDT", true, 10);
    auto strategy = std::make_unique<MockStrategy>(ctx);
    manager.register_strategy(std::move(strategy));

    manager.set_signal_callback([](const TradingSignal &) {});
    manager.start();

    // Snapshot while threads may be running (they exit quickly without market feed).
    // The enabled strategy's active flag is set to true in start().
    const auto snaps = manager.snapshot();
    ASSERT_EQ(snaps.size(), 1u);
    EXPECT_EQ(snaps[0].symbol, "BTC_USDT");
    EXPECT_TRUE(snaps[0].enabled);
    // Note: running may be true or false here depending on timing
    // (the thread exits quickly because no market feed is available).

    manager.stop();

    // After stop(), running must be false.
    const auto snaps_after = manager.snapshot();
    ASSERT_EQ(snaps_after.size(), 1u);
    EXPECT_FALSE(snaps_after[0].running);
}
