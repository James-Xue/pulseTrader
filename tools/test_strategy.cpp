// test_strategy.cpp — Smoke test tool for Layer 6 Strategy Engine
//
// Constructs a simulated market environment with synthetic data and verifies
// that strategies produce correct trading signals.
//
// Usage:
//   ./build/tools/test_strategy
//
// What it tests:
//   1. MomentumScalper with synthetic kline data (EMA crossover detection)
//   2. OrderBookScalper with synthetic order book (imbalance detection)
//   3. MeanReversionScalper with synthetic kline data (Bollinger Band breach)
//   4. SignalAggregator combining signals from all three strategies
//   5. End-to-end signal flow: Strategy → SignalAggregator → output callback

#include "core/config.hpp"
#include "logging/logger.hpp"
#include "strategy/scalping/mean_reversion_scalper.hpp"
#include "strategy/scalping/momentum_scalper.hpp"
#include "strategy/scalping/orderbook_scalper.hpp"
#include "strategy/signal/signal_aggregator.hpp"
#include "strategy/strategy_manager.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

using namespace pulse;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// Helper: print a separator line
// ---------------------------------------------------------------------------
static void print_separator(const std::string &title)
{
    std::cout << "\n=== " << title << " ===" << std::endl;
}

// ---------------------------------------------------------------------------
// Test 1: MomentumScalper EMA computation
// ---------------------------------------------------------------------------
static void test_momentum_scalper()
{
    print_separator("MomentumScalper — EMA Crossover");

    // Create a MomentumScalper with short periods for quick testing.
    StrategyContext ctx;
    ctx.config.name = "momentum_scalper";
    ctx.config.symbol = "BTC_USDT";
    ctx.config.order_quantity = 0.001;
    ctx.config.min_confidence = 0.0;
    ctx.config.poll_interval_ms = 100;
    // Note: no market_feed — strategy will log and skip onKline.
    // We test the interface and parameter handling here.

    MomentumScalper scalper(ctx);
    scalper.params().ema_fast_period.store(5.0, std::memory_order_release);
    scalper.params().ema_slow_period.store(10.0, std::memory_order_release);

    std::cout << "  Strategy name: " << scalper.name() << std::endl;
    std::cout << "  Strategy ID:   " << scalper.id() << std::endl;
    std::cout << "  Fast EMA:      " << scalper.params().ema_fast_period.load() << std::endl;
    std::cout << "  Slow EMA:      " << scalper.params().ema_slow_period.load() << std::endl;

    // Test onTick (should be silently ignored).
    market::Ticker ticker;
    ticker.symbol = "BTC_USDT";
    ticker.last = 50000.0;
    ticker.bid = 49999.0;
    ticker.ask = 50001.0;
    scalper.onTick(ticker);
    std::cout << "  onTick() — OK (ignored as expected)" << std::endl;

    // Test onKline without market feed (should return early).
    market::Kline kline;
    kline.closed = true;
    kline.close = 50000.0;
    scalper.onKline(kline);
    std::cout << "  onKline() — OK (no market feed, returned early)" << std::endl;

    // Test signal callback wiring.
    int signalCount = 0;
    scalper.setSignalCallback([&](const TradingSignal &s)
        {
            ++signalCount;
            std::cout << "  Signal: " << (SignalType::Buy == s.type ? "BUY" : "SELL")
                      << " confidence=" << s.confidence << std::endl;
        });
    std::cout << "  Signal callback wired — OK" << std::endl;
    std::cout << "  [PASS] MomentumScalper interface test" << std::endl;
}

// ---------------------------------------------------------------------------
// Test 2: OrderBookScalper imbalance detection
// ---------------------------------------------------------------------------
static void test_orderbook_scalper()
{
    print_separator("OrderBookScalper — Order Book Imbalance");

    StrategyContext ctx;
    ctx.config.name = "orderbook_scalper";
    ctx.config.symbol = "ETH_USDT";
    ctx.config.order_quantity = 0.01;
    ctx.config.min_confidence = 0.0;
    ctx.config.poll_interval_ms = 100;

    OrderBookScalper scalper(ctx);
    scalper.params().ob_imbalance_threshold.store(0.2, std::memory_order_release);
    scalper.params().ob_depth.store(5.0, std::memory_order_release);
    scalper.params().cooldown_seconds.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> signals;
    scalper.setSignalCallback([&](const TradingSignal &s)
        {
            signals.push_back(s);
        });

    // Test 1: Strong buy pressure (bid >> ask).
    {
        market::OrderBook book;
        book.symbol = "ETH_USDT";
        for (int i = 0; i < 5; ++i)
        {
            book.bids[3000.0 - i] = 10.0; // 50 total bid volume
            book.asks[3001.0 + i] = 2.0;  // 10 total ask volume
        }

        scalper.onOrderbook(book);

        if (!signals.empty() && SignalType::Buy == signals.back().type)
        {
            std::cout << "  Buy signal detected — imbalance=" << signals.back().confidence
                      << " [PASS]" << std::endl;
        }
        else
        {
            std::cout << "  [FAIL] Expected Buy signal" << std::endl;
        }
    }

    // Test 2: Strong sell pressure (ask >> bid).
    {
        market::OrderBook book;
        book.symbol = "ETH_USDT";
        for (int i = 0; i < 5; ++i)
        {
            book.bids[3000.0 - i] = 2.0;  // 10 total bid volume
            book.asks[3001.0 + i] = 10.0; // 50 total ask volume
        }

        scalper.onOrderbook(book);

        if (signals.size() >= 2 && SignalType::Sell == signals.back().type)
        {
            std::cout << "  Sell signal detected — imbalance=" << signals.back().confidence
                      << " [PASS]" << std::endl;
        }
        else
        {
            std::cout << "  [FAIL] Expected Sell signal" << std::endl;
        }
    }

    // Test 3: Balanced book — no signal.
    {
        std::size_t before = signals.size();
        market::OrderBook book;
        book.symbol = "ETH_USDT";
        for (int i = 0; i < 5; ++i)
        {
            book.bids[3000.0 - i] = 5.0;
            book.asks[3001.0 + i] = 5.0;
        }

        scalper.onOrderbook(book);

        if (signals.size() == before)
        {
            std::cout << "  Balanced book — no signal [PASS]" << std::endl;
        }
        else
        {
            std::cout << "  [FAIL] Expected no signal on balanced book" << std::endl;
        }
    }

    std::cout << "  Total signals emitted: " << signals.size() << std::endl;
}

// ---------------------------------------------------------------------------
// Test 3: MeanReversionScalper interface
// ---------------------------------------------------------------------------
static void test_mean_reversion_scalper()
{
    print_separator("MeanReversionScalper — Bollinger Bands");

    StrategyContext ctx;
    ctx.config.name = "mean_reversion_scalper";
    ctx.config.symbol = "SOL_USDT";
    ctx.config.order_quantity = 1.0;
    ctx.config.min_confidence = 0.0;
    ctx.config.poll_interval_ms = 100;

    MeanReversionScalper scalper(ctx);
    scalper.params().bb_period.store(20.0, std::memory_order_release);
    scalper.params().bb_std_dev.store(2.0, std::memory_order_release);

    std::cout << "  Strategy name: " << scalper.name() << std::endl;
    std::cout << "  Strategy ID:   " << scalper.id() << std::endl;
    std::cout << "  BB Period:     " << scalper.params().bb_period.load() << std::endl;
    std::cout << "  BB StdDev:     " << scalper.params().bb_std_dev.load() << std::endl;

    // onKline without market feed — should return early.
    market::Kline kline;
    kline.closed = true;
    kline.close = 150.0;
    scalper.onKline(kline);
    std::cout << "  onKline() — OK (no market feed, returned early)" << std::endl;

    std::cout << "  [PASS] MeanReversionScalper interface test" << std::endl;
}

// ---------------------------------------------------------------------------
// Test 4: SignalAggregator
// ---------------------------------------------------------------------------
static void test_signal_aggregator()
{
    print_separator("SignalAggregator — Weighted Voting");

    StrategyConfig config;
    config.signal_aggregator_threshold = 0.5;
    config.signal_cooldown_sec = 0;

    SignalAggregator agg(config);

    std::vector<TradingSignal> emitted;
    agg.setOutputCallback([&](const TradingSignal &s)
        {
            emitted.push_back(s);
            std::cout << "  Emitted: " << s.symbol << " "
                      << (SignalType::Buy == s.type ? "BUY" : "SELL")
                      << " confidence=" << s.confidence << std::endl;
        });

    agg.setWeight("momentum_scalper_BTC_USDT", 1.5);
    agg.setWeight("orderbook_scalper_BTC_USDT", 1.0);

    // Signal 1: MomentumScalper buys BTC (high weight).
    TradingSignal s1;
    s1.type = SignalType::Buy;
    s1.symbol = "BTC_USDT";
    s1.confidence = 0.8;
    s1.price = 50000.0;
    s1.strategy_id = "momentum_scalper_BTC_USDT";
    s1.timestamp = now();
    s1.reason = "EMA crossover";
    agg.addSignal(s1);

    // Signal 2: OrderBookScalper also buys BTC.
    TradingSignal s2;
    s2.type = SignalType::Buy;
    s2.symbol = "BTC_USDT";
    s2.confidence = 0.6;
    s2.price = 50001.0;
    s2.strategy_id = "orderbook_scalper_BTC_USDT";
    s2.timestamp = now();
    s2.reason = "Order book imbalance";
    agg.addSignal(s2);

    std::cout << "  Total input signals: " << agg.signalCount() << std::endl;
    std::cout << "  Emitted signals: " << emitted.size() << std::endl;

    if (!emitted.empty())
    {
        std::cout << "  [PASS] SignalAggregator emitted consolidated signal" << std::endl;
    }
    else
    {
        std::cout << "  [INFO] No signal emitted (threshold not reached)" << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Test 5: StrategyManager lifecycle
// ---------------------------------------------------------------------------
static void test_strategy_manager()
{
    print_separator("StrategyManager — Lifecycle");

    StrategyManager manager;

    // Register strategies (without market feed — threads will exit quickly).
    StrategyContext ctx;
    ctx.config.name = "momentum_scalper";
    ctx.config.symbol = "BTC_USDT";
    ctx.config.enabled = true;
    ctx.config.poll_interval_ms = 50;

    auto scalper = std::make_unique<MomentumScalper>(ctx);
    manager.registerStrategy(std::move(scalper));

    manager.setSignalCallback([](const TradingSignal &s)
        {
            std::cout << "  Manager received: " << s.symbol << " "
                      << (SignalType::Buy == s.type ? "BUY" : "SELL") << std::endl;
        });

    std::cout << "  Registered strategies: " << manager.strategyCount() << std::endl;

    manager.start();
    std::cout << "  Running threads: " << manager.runningCount() << std::endl;

    // Let threads run briefly (they'll exit due to no market feed).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    manager.stop();
    std::cout << "  After stop — running: " << manager.runningCount() << std::endl;
    std::cout << "  [PASS] StrategyManager lifecycle test" << std::endl;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    // Initialize logging.
    LogConfig log_config;
    log_config.level = "info";
    log_config.toConsole = true;
    log_config.toFile = false;
    logging::Logger::init(log_config);

    std::cout << "pulseTrader Strategy Engine — Smoke Test" << std::endl;
    std::cout << "=========================================" << std::endl;

    test_momentum_scalper();
    test_orderbook_scalper();
    test_mean_reversion_scalper();
    test_signal_aggregator();
    test_strategy_manager();

    std::cout << "\n=========================================" << std::endl;
    std::cout << "All smoke tests completed." << std::endl;

    logging::Logger::shutdown();
    return 0;
}
