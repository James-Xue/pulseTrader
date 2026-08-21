// test_eth_scalper.cpp — Unit tests for the ETH short-only strategy
//
// Full kline pipeline (FeedHarness → KlineBuffer → onKline): the first
// strategy tests that exercise the complete signal path, closing the
// historical "kline full-chain untested in unit tests" gap.
//
// Test data recipe (fast=2, slow=3; klineNeeded=15, warmup=3):
//   - Call A: 15 flat candles @10 (high 10.5 / low 9.5, range 1.0)
//     → EMA fast = slow = 10, first evaluation (m_hasPrev=false) → no cross.
//   - Call B: +1 candle @5 → snapshot = [10×14, 5]
//     → fast EMA = 5·(2/3) + 10·(1/3) ≈ 6.667, slow EMA = 5·0.5 + 10·0.5 = 7.5
//     → bearish crossover (prev fast >= prev slow, fast < slow) → Sell.

#include "strategy/scalping/EthScalper.hpp"
#include "strategy/scalping/UnifiedScalper.hpp"

#include "exchange/GateRestClient.hpp"
#include "market/MarketFeed.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

using namespace pulse;
using namespace pulse::strategy;

namespace
{

// EmaProbe — exposes the protected static computeEma for expected-value
// computation in tests.
class EmaProbe : public UnifiedScalper
{
  public:
    using UnifiedScalper::UnifiedScalper;
    using UnifiedScalper::computeEma;
};

// ---------------------------------------------------------------------------
// FeedHarness — MarketFeed constructible without network I/O.
// ---------------------------------------------------------------------------
struct FeedHarness
{
    exchange::GateRestClient rest;
    market::MarketFeed feed;

    explicit FeedHarness(MarketType market_type)
        : rest{ ExchangeConfig{}, market_type }
        , feed{ nullptr, rest, market_type }
    {
    }
};

// Build `count` candles; first `flat` of them at `flat_price`, the rest at
// `drop_price` (range ±0.5 around close).
static std::vector<market::Kline> make_candles(std::size_t flat, double flat_price,
    std::size_t drop, double drop_price, double drop_high = 0.0)
{
    std::vector<market::Kline> candles;
    candles.reserve(flat + drop);
    for (std::size_t i = 0; i < flat; ++i)
    {
        market::Kline k;
        k.open = flat_price;
        k.high = flat_price + 0.5;
        k.low = flat_price - 0.5;
        k.close = flat_price;
        k.closed = true;
        candles.push_back(k);
    }
    for (std::size_t i = 0; i < drop; ++i)
    {
        market::Kline k;
        k.open = drop_price;
        // Spike variant: override high (default = close + 0.5).
        k.high = (drop_high > 0.0) ? drop_high : drop_price + 0.5;
        k.low = drop_price - 0.5;
        k.close = drop_price;
        k.closed = true;
        candles.push_back(k);
    }
    return candles;
}

static StrategyContext make_ctx(FeedHarness &harness,
    std::map<std::string, double> custom = {})
{
    StrategyContext ctx;
    ctx.config.name = "eth_scalper";
    ctx.config.symbol = "ETH_USDT";
    ctx.config.custom_params = std::move(custom);
    ctx.market_feed = &harness.feed;
    return ctx;
}

// Drive the two-phase evaluation:
//   - push `flat` candles → onKline (call A: builds prev EMA state at 10/10)
//   - push `drop` candles → onKline (call B: detects the crossover)
static void drive_two_calls(FeedHarness &harness, EthScalper &scalper,
    const std::vector<market::Kline> &flat, const std::vector<market::Kline> &drop)
{
    auto &buf = harness.feed.getKlineBuffer("ETH_USDT");
    for (const auto &c : flat)
    {
        buf.push(c);
    }
    market::Kline trigger;
    trigger.closed = true;
    scalper.onKline(trigger);
    for (const auto &c : drop)
    {
        buf.push(c);
    }
    scalper.onKline(trigger);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(EthScalper, NameAndId)
{
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<EthScalper>(make_ctx(harness));
    EXPECT_EQ("EthScalper", scalper->name());
    EXPECT_EQ("eth_scalper_ETH_USDT", scalper->id());
}

TEST(EthScalper, NoSignalWithoutFeed)
{
    StrategyContext ctx;
    ctx.config.name = "eth_scalper";
    ctx.config.symbol = "ETH_USDT";

    auto scalper = std::make_unique<EthScalper>(ctx);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    market::Kline kline;
    kline.closed = true;
    scalper->onKline(kline);
    scalper->onTick(market::Ticker{});

    EXPECT_TRUE(received.empty());
}

TEST(EthScalper, BearishCrossEmitsShortSignal)
{
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<EthScalper>(make_ctx(harness));
    scalper->params().ema_fast_period.store(2.0, std::memory_order_release);
    scalper->params().ema_slow_period.store(3.0, std::memory_order_release);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    drive_two_calls(harness, *scalper,
        make_candles(15, 10.0, 0, 0.0), make_candles(0, 0.0, 1, 5.0));

    ASSERT_EQ(1u, received.size());
    const auto &sig = received.front();
    EXPECT_EQ(SignalType::Sell, sig.type);
    EXPECT_EQ("ETH_USDT", sig.symbol);
    EXPECT_EQ("eth_scalper_ETH_USDT", sig.strategy_id);
    EXPECT_DOUBLE_EQ(5.0, sig.price);
    EXPECT_GT(sig.confidence, 0.0);
    EXPECT_LE(sig.confidence, 1.0);
    EXPECT_NE(std::string::npos, sig.reason.find("bearish"));

    // Confidence formula: clamp(|fast - slow| / atr × scale, 0, 1) with the
    // same EMA computation the strategy used (prev = 10 from call A).
    const auto &ind = sig.indicators;
    const double atr = ind.value("atr", 0.0);
    ASSERT_GT(atr, 0.0);
    std::vector<double> closes(15, 10.0);
    closes.back() = 5.0;
    const double exp_fast = EmaProbe::computeEma(closes, 2.0, 10.0);
    const double exp_slow = EmaProbe::computeEma(closes, 3.0, 10.0);
    const double exp_conf = std::clamp(std::abs(exp_fast - exp_slow) / atr, 0.0, 1.0);
    EXPECT_NEAR(exp_conf, sig.confidence, 1e-9);

    // ATR-adaptive suggested TP (default eth_atr_step = 0.05).
    EXPECT_DOUBLE_EQ(0.05, ind.value("atr_step", 0.0));
    EXPECT_NEAR(5.0 - 0.05 * atr, ind.value("suggested_tp", 0.0), 1e-9);
    EXPECT_NEAR(1.0, ind.value("spike_range_usd", 0.0), 1e-9); // Normal range passes filter.
    EXPECT_DOUBLE_EQ(120.0, ind.value("spike_filter_usd", 0.0));
}

TEST(EthScalper, BullishCrossNoSignal)
{
    // Rising market (fast crosses ABOVE slow) → short-only strategy stays silent.
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<EthScalper>(make_ctx(harness));
    scalper->params().ema_fast_period.store(2.0, std::memory_order_release);
    scalper->params().ema_slow_period.store(3.0, std::memory_order_release);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    drive_two_calls(harness, *scalper,
        make_candles(15, 10.0, 0, 0.0), make_candles(0, 0.0, 1, 15.0));

    EXPECT_TRUE(received.empty());
}

TEST(EthScalper, SpikeFilterBlocksEntry)
{
    // Last candle high=130 → range 126 > default filter 120 → 暴拉, no chase.
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<EthScalper>(make_ctx(harness));
    scalper->params().ema_fast_period.store(2.0, std::memory_order_release);
    scalper->params().ema_slow_period.store(3.0, std::memory_order_release);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    drive_two_calls(harness, *scalper,
        make_candles(15, 10.0, 0, 0.0), make_candles(0, 0.0, 1, 5.0, /*drop_high=*/130.0));

    EXPECT_TRUE(received.empty());
}

TEST(EthScalper, AtrAdaptiveStep)
{
    // eth_atr_step = 0.1 → suggested_tp = close - 0.1 × atr.
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<EthScalper>(
        make_ctx(harness, { { "eth_atr_step", 0.1 } }));
    scalper->params().ema_fast_period.store(2.0, std::memory_order_release);
    scalper->params().ema_slow_period.store(3.0, std::memory_order_release);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    drive_two_calls(harness, *scalper,
        make_candles(15, 10.0, 0, 0.0), make_candles(0, 0.0, 1, 5.0));

    ASSERT_EQ(1u, received.size());
    const auto &ind = received.front().indicators;
    const double atr = ind.value("atr", 0.0);
    EXPECT_DOUBLE_EQ(0.1, ind.value("atr_step", 0.0));
    EXPECT_NEAR(5.0 - 0.1 * atr, ind.value("suggested_tp", 0.0), 1e-9);
}

TEST(EthScalper, CustomParamsControlBehavior)
{
    // Same data, different spike filter: 0.5 blocks the range-1.0 candle,
    // while the default 120.0 lets it through.
    auto run = [](double filter) -> std::size_t
    {
        FeedHarness harness{ MarketType::Futures };
        auto scalper = std::make_unique<EthScalper>(
            make_ctx(harness, { { "eth_spike_filter_usd", filter } }));
        scalper->params().ema_fast_period.store(2.0, std::memory_order_release);
        scalper->params().ema_slow_period.store(3.0, std::memory_order_release);
        scalper->params().min_confidence.store(0.0, std::memory_order_release);

        std::vector<TradingSignal> received;
        scalper->setSignalCallback([&](const TradingSignal &s)
            {
                received.push_back(s);
            });

        drive_two_calls(harness, *scalper,
            make_candles(15, 10.0, 0, 0.0), make_candles(0, 0.0, 1, 5.0));
        return received.size();
    };

    EXPECT_EQ(0u, run(0.5));  // Range 1.0 > 0.5 → blocked.
    EXPECT_EQ(1u, run(120.0)); // Range 1.0 <= 120.0 → allowed.
}

TEST(EthScalper, CustomParamDefaults)
{
    // No custom_params configured → default values surface in indicators.
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<EthScalper>(make_ctx(harness));
    scalper->params().ema_fast_period.store(2.0, std::memory_order_release);
    scalper->params().ema_slow_period.store(3.0, std::memory_order_release);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    drive_two_calls(harness, *scalper,
        make_candles(15, 10.0, 0, 0.0), make_candles(0, 0.0, 1, 5.0));

    ASSERT_EQ(1u, received.size());
    const auto &ind = received.front().indicators;
    EXPECT_DOUBLE_EQ(0.05, ind.value("atr_step", 0.0));
    EXPECT_DOUBLE_EQ(120.0, ind.value("spike_filter_usd", 0.0));
}

TEST(EthScalper, ConfidenceScaleApplied)
{
    // scale = 0.5 halves the confidence vs scale = 1.0 on identical data.
    auto run = [](double scale) -> double
    {
        FeedHarness harness{ MarketType::Futures };
        auto scalper = std::make_unique<EthScalper>(
            make_ctx(harness, { { "eth_min_confidence_scale", scale } }));
        scalper->params().ema_fast_period.store(2.0, std::memory_order_release);
        scalper->params().ema_slow_period.store(3.0, std::memory_order_release);
        scalper->params().min_confidence.store(0.0, std::memory_order_release);

        std::vector<TradingSignal> received;
        scalper->setSignalCallback([&](const TradingSignal &s)
            {
                received.push_back(s);
            });

        drive_two_calls(harness, *scalper,
            make_candles(15, 10.0, 0, 0.0), make_candles(0, 0.0, 1, 5.0));
        return received.empty() ? 0.0 : received.front().confidence;
    };

    const double full = run(1.0);
    const double half = run(0.5);
    EXPECT_GT(full, 0.0);
    EXPECT_NEAR(full * 0.5, half, 1e-9);
}
