// test_replay_driver.cpp — ReplayDriver end-to-end against real strategies,
// using the FeedHarness construction (no I/O, no threads).

#include "backtest/BacktestAccount.hpp"
#include "backtest/ReplayDriver.hpp"

#include "strategy/StrategyRegistry.hpp"

#include <gtest/gtest.h>

namespace pulse::backtest::test
{

namespace
{

BacktestOptions makeOpts(const std::string &strategy_name, const std::string &symbol)
{
    BacktestOptions opts;
    opts.strategy_name = strategy_name;
    opts.symbol = symbol;
    opts.market_type = MarketType::Futures;
    opts.order_quantity = 20.0;
    opts.quanto_multiplier = 0.01;
    opts.taker_fee_rate = 0.0005;
    opts.min_confidence = 0.0; // let every signal through
    return opts;
}

/// Synthetic trend: 250 candles stepping up, then 250 stepping down.
/// 1m interval starting at a fixed epoch.
std::vector<market::Kline> makeTrendCandles(std::size_t n, double start, double step)
{
    std::vector<market::Kline> candles;
    candles.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double close = start + static_cast<double>(i) * step;
        market::Kline k;
        k.open_time = 1'700'000'000'000LL + static_cast<std::int64_t>(i) * 60'000;
        k.close_time = k.open_time + 60'000;
        k.open = close - step;
        k.high = close + step * 0.5;
        k.low = close - step * 1.5;
        k.close = close;
        k.volume = 100.0;
        k.closed = true;
        candles.push_back(k);
    }
    return candles;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ReplayDriver — warmup counting + signal collection through a real strategy
// ---------------------------------------------------------------------------

TEST(ReplayDriverTest, MomentumReplay_ProducesSignalsAndWarmup)
{
    auto registry = strategy::makeBuiltinStrategyRegistry();
    BacktestOptions opts = makeOpts("momentum_scalper", "ETH_USDT");
    ReplayDriver driver(opts, registry);
    BacktestAccount account(opts);

    // 500 candles: 250 up, then 250 down → the momentum strategy sees a
    // fast/slow cross in each direction.
    auto trend = makeTrendCandles(250, 2000.0, 0.5);
    auto down = makeTrendCandles(250, 2125.0, -0.5);
    trend.insert(trend.end(), down.begin(), down.end());

    const auto result = driver.run(trend, account);
    ASSERT_TRUE(ok(result));

    EXPECT_EQ(500u, value(result).candles_fed);
    // Momentum warmupThreshold = 21 → evaluation starts at candle 20, so the
    // first signal can never precede candle 20; a first-signal warmup >= 20.
    EXPECT_GE(value(result).warmup_candles, 20u);
    EXPECT_LE(value(result).warmup_candles, 500u);
    EXPECT_FALSE(value(result).signals.empty());
    EXPECT_TRUE(account.entrySignalCount() > 0 || account.ignoredSignalCount() > 0);
}

TEST(ReplayDriverTest, EmaResonance_FiresOnRegimeTransition)
{
    auto registry = strategy::makeBuiltinStrategyRegistry();
    BacktestOptions opts = makeOpts("ema_resonance_scalper", "ETH_USDT");
    ReplayDriver driver(opts, registry);
    BacktestAccount account(opts);

    // EmaResonance is transition-triggered: a monotonic trend that aligns
    // from the first evaluation NEVER fires (the regime never changes). To
    // force a signal: flat (no alignment) → ramp (bull alignment), which is
    // a None → Bull transition. NOTE: makeTrendCandles opens at close-step,
    // so the first ramp candle still closes at the flat price — the first
    // non-flat close (2001.0) is the second ramp candle.
    std::vector<market::Kline> candles = makeTrendCandles(250, 2000.0, 0.0);
    auto ramp = makeTrendCandles(120, 2000.0, 1.0);
    candles.insert(candles.end(), ramp.begin(), ramp.end());

    const auto result = driver.run(candles, account);
    ASSERT_TRUE(ok(result));

    // klineNeeded = warmupThreshold = 201 → evaluation starts at candle 200.
    // Candles 200..250 evaluate as None (flat); candle 251 is the first bar
    // whose close moved → None→Bull transition fires the Buy at 2001.0.
    EXPECT_EQ(251u, value(result).warmup_candles);
    ASSERT_FALSE(value(result).signals.empty());
    EXPECT_EQ(strategy::SignalType::Buy, value(result).signals.front().type);
    EXPECT_DOUBLE_EQ(2001.0, value(result).signals.front().price);

    // Flip back: ramp down from the ramp's last close → Bull→Bear direct
    // flip also fires (Sell). Note the strategy commits its regime on every
    // evaluation, so replaying the flat section resets prev to None and the
    // up-ramp re-fires the Buy transition before the down-ramp fires Sell.
    auto down = makeTrendCandles(120, 2120.0, -1.0);
    candles.insert(candles.end(), down.begin(), down.end());
    const auto result2 = driver.run(candles, account);
    ASSERT_TRUE(ok(result2));
    ASSERT_GE(value(result2).signals.size(), 2u);
    EXPECT_EQ(strategy::SignalType::Buy, value(result2).signals[0].type);
    EXPECT_EQ(strategy::SignalType::Sell, value(result2).signals[1].type);
}

TEST(ReplayDriverTest, TooFewCandles_NoWarmupNoSignals)
{
    auto registry = strategy::makeBuiltinStrategyRegistry();
    BacktestOptions opts = makeOpts("ema_resonance_scalper", "ETH_USDT");
    ReplayDriver driver(opts, registry);
    BacktestAccount account(opts);

    const auto result = driver.run(makeTrendCandles(50, 2000.0, 1.0), account);
    ASSERT_TRUE(ok(result));
    EXPECT_TRUE(value(result).signals.empty());
    EXPECT_EQ(50u, value(result).warmup_candles); // never warmed up → fed count
    EXPECT_FALSE(account.hasPosition());
}

TEST(ReplayDriverTest, SignalPriceIsCandleClose_AndTimestampIsCandleOpen)
{
    auto registry = strategy::makeBuiltinStrategyRegistry();
    BacktestOptions opts = makeOpts("ema_resonance_scalper", "ETH_USDT");
    ReplayDriver driver(opts, registry);
    BacktestAccount account(opts);

    // Flat 260 → ramp 40. First non-flat close (2001.0) is candle 261.
    std::vector<market::Kline> candles = makeTrendCandles(260, 2000.0, 0.0);
    auto ramp = makeTrendCandles(40, 2000.0, 1.0);
    candles.insert(candles.end(), ramp.begin(), ramp.end());

    const auto result = driver.run(candles, account);
    ASSERT_TRUE(ok(result));

    const auto &signals = value(result).signals;
    ASSERT_FALSE(signals.empty());
    const auto first = signals.front();
    // Timestamps are candle open times in ms, aligned to the 60s grid and
    // inside the fed range — never wall clock.
    EXPECT_EQ(0LL, (first.candle_open_ms - 1'700'000'000'000LL) % 60'000);
    EXPECT_GE(first.candle_open_ms, 1'700'000'000'000LL);
    EXPECT_LE(first.candle_open_ms, 1'700'000'000'000LL + 300LL * 60'000);
    // The strategy fills price with the candle close of the evaluation bar.
    EXPECT_DOUBLE_EQ(2001.0, first.price);
}

} // namespace pulse::backtest::test
