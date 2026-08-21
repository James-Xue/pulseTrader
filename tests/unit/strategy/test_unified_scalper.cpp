// test_unified_scalper.cpp — Unit tests for the UnifiedScalper template-method base
//
// Covers the base-class contract that the migrated scalpers share:
//   - ATR computation (the legacy three-copy formula, now in one place)
//   - warmup gate (candles < threshold → no evaluation, no signal)
//   - cooldown gate (blocks the signal but NOT the evaluateEntry state commit)
//   - base-filled identity fields (symbol / strategy_id / timestamp)
//   - passive default (evaluateEntry → nullopt) + name/id defaults used by
//     the StrategyRegistry fallback

#include "strategy/scalping/UnifiedScalper.hpp"

#include "exchange/GateRestClient.hpp"
#include "market/MarketFeed.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

using namespace pulse;
using namespace pulse::strategy;

namespace
{

// ---------------------------------------------------------------------------
// FeedHarness — MarketFeed constructible without network I/O (start() is
// never called; the REST client is never actually used). Pattern copied from
// tests/unit/market/test_market_feed_sink.cpp.
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

// ---------------------------------------------------------------------------
// TestScalper — emits one Buy entry per evaluation (evaluation counter lets
// tests assert the state-commit-on-cooldown contract).
// ---------------------------------------------------------------------------
class TestScalper : public UnifiedScalper
{
  public:
    using UnifiedScalper::UnifiedScalper;

    std::size_t eval_calls{ 0 };
    bool cooldown_flag{ true };

  protected:
    [[nodiscard]] std::string className() const override { return "TestScalper"; }
    [[nodiscard]] std::string idPrefix() const override { return "test_scalper"; }
    [[nodiscard]] std::size_t klineNeeded() const override { return 2; }
    [[nodiscard]] bool cooldownEnabled() const override { return cooldown_flag; }

    std::optional<EntryContext> evaluateEntry(
        const std::vector<market::Kline> &candles) override
    {
        ++eval_calls;
        if (candles.empty())
        {
            return std::nullopt;
        }
        EntryContext e;
        e.type = SignalType::Buy;
        e.price = candles.back().close;
        e.confidence = 0.7; // Above the 0.6 min_confidence default gate.
        e.reason = "test entry";
        e.indicators = { { "k", 1.0 } };
        return e;
    }
};

// AtrProbe — exposes the protected computeAtr / customParam for testing.
class AtrProbe : public UnifiedScalper
{
  public:
    using UnifiedScalper::UnifiedScalper;
    using UnifiedScalper::computeAtr;
    using UnifiedScalper::customParam;
};

// Context wired to a FeedHarness (the strategy template reads the feed from
// m_context.market_feed).
static StrategyContext make_ctx(FeedHarness &harness, const std::string &symbol)
{
    StrategyContext ctx;
    ctx.config.name = "test_scalper";
    ctx.config.symbol = symbol;
    ctx.market_feed = &harness.feed;
    return ctx;
}

// Build a candle series; closes are linear so ATR = (high-low) spread.
static std::vector<market::Kline> make_candles(std::size_t count, double close_step = 1.0)
{
    std::vector<market::Kline> candles;
    candles.reserve(count);
    double price = 100.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        market::Kline k;
        k.open = price;
        k.high = price + 2.0;
        k.low = price - 2.0;
        k.close = price + close_step;
        k.closed = true;
        candles.push_back(k);
        price += close_step;
    }
    return candles;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(UnifiedScalper, NameAndIdDefaults)
{
    StrategyContext ctx;
    ctx.config.name = "bogus_scalper";
    ctx.config.symbol = "BTC_USDT";

    auto scalper = std::make_unique<UnifiedScalper>(ctx);
    EXPECT_EQ("UnifiedScalper", scalper->name());
    EXPECT_EQ("bogus_scalper_BTC_USDT", scalper->id());
}

TEST(UnifiedScalper, PassiveDefaultNeverEmits)
{
    FeedHarness harness{ MarketType::Futures };
    harness.feed.getKlineBuffer("BTC_USDT").push(make_candles(2).front());

    StrategyContext ctx;
    ctx.config.name = "bogus_scalper";
    ctx.config.symbol = "BTC_USDT";
    ctx.market_feed = &harness.feed;

    auto scalper = std::make_unique<UnifiedScalper>(ctx);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // The default evaluateEntry returns nullopt, so nothing is ever emitted
    // even with a live feed and enough candles.
    market::Kline kline;
    kline.closed = true;
    scalper->onKline(kline);

    EXPECT_TRUE(received.empty());
}

TEST(UnifiedScalper, ComputeAtrInsufficientData)
{
    StrategyContext ctx;
    AtrProbe probe{ ctx };

    // period + 1 candles required; exactly `period` → 0.0.
    auto candles = make_candles(10);
    EXPECT_DOUBLE_EQ(0.0, probe.computeAtr(candles, 10));
}

TEST(UnifiedScalper, ComputeAtrMatchesLegacyFormula)
{
    StrategyContext ctx;
    AtrProbe probe{ ctx };

    // Candles with high=close+2, low=close-2: TR = 4 for every candle, so
    // ATR over any period with enough data is exactly 4.0.
    auto candles = make_candles(15);
    EXPECT_DOUBLE_EQ(4.0, probe.computeAtr(candles, 14));
}

TEST(UnifiedScalper, WarmupGateBlocksEvaluation)
{
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<TestScalper>(make_ctx(harness, "BTC_USDT"));
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // One candle in the buffer (klineNeeded = 2) → warmup gate, no evaluation.
    auto candles = make_candles(1);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles.front());
    market::Kline trigger;
    trigger.closed = true;
    scalper->onKline(trigger);
    EXPECT_EQ(0u, scalper->eval_calls);
    EXPECT_TRUE(received.empty());

    // Second candle crosses the threshold → evaluation runs, signal emitted.
    harness.feed.getKlineBuffer("BTC_USDT").push(make_candles(1, 2.0).front());
    scalper->onKline(trigger);
    EXPECT_EQ(1u, scalper->eval_calls);
    EXPECT_EQ(1u, received.size());
}

TEST(UnifiedScalper, FullSignalFlowFillsBaseFields)
{
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<TestScalper>(make_ctx(harness, "ETH_USDT"));
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    auto candles = make_candles(2);
    harness.feed.getKlineBuffer("ETH_USDT").push(candles[0]);
    harness.feed.getKlineBuffer("ETH_USDT").push(candles[1]);

    market::Kline trigger;
    trigger.closed = true;
    scalper->onKline(trigger);

    ASSERT_EQ(1u, received.size());
    const auto &sig = received.front();
    EXPECT_EQ(SignalType::Buy, sig.type);
    EXPECT_EQ("ETH_USDT", sig.symbol);
    EXPECT_EQ("test_scalper_ETH_USDT", sig.strategy_id);
    EXPECT_NE(0, sig.timestamp.time_since_epoch().count());
    EXPECT_DOUBLE_EQ(102.0, sig.price); // candles[1] close = 101 + 1.0
    EXPECT_DOUBLE_EQ(1.0, sig.indicators.value("k", 0.0));
}

TEST(UnifiedScalper, CooldownBlocksSecondSignalButStillEvaluates)
{
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<TestScalper>(make_ctx(harness, "BTC_USDT"));
    scalper->params().min_confidence.store(0.0, std::memory_order_release);
    scalper->params().cooldown_seconds.store(60.0, std::memory_order_release); // 60 s cooldown

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    auto candles = make_candles(2);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[0]);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[1]);

    market::Kline trigger;
    trigger.closed = true;
    scalper->onKline(trigger); // First pass → signal.
    scalper->onKline(trigger); // Immediate second pass → cooldown blocks signal.

    EXPECT_EQ(1u, received.size());
    // The state-commit contract: evaluateEntry still ran on the blocked pass
    // (legacy SuperTrend committed rolling state regardless of cooldown).
    EXPECT_EQ(2u, scalper->eval_calls);
}

TEST(UnifiedScalper, CooldownDisabledByZeroParam)
{
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<TestScalper>(make_ctx(harness, "BTC_USDT"));
    scalper->params().min_confidence.store(0.0, std::memory_order_release);
    scalper->params().cooldown_seconds.store(0.0, std::memory_order_release); // disabled

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    auto candles = make_candles(2);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[0]);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[1]);

    market::Kline trigger;
    trigger.closed = true;
    scalper->onKline(trigger);
    scalper->onKline(trigger);

    EXPECT_EQ(2u, received.size());
}

TEST(UnifiedScalper, CustomParamReturnsConfiguredValue)
{
    StrategyContext ctx;
    ctx.config.custom_params["eth_atr_step"] = 0.05;
    AtrProbe probe{ ctx };
    EXPECT_DOUBLE_EQ(0.05, probe.customParam("eth_atr_step", 0.5));
}

TEST(UnifiedScalper, CustomParamFallsBackToDefault)
{
    StrategyContext ctx; // No custom_params configured.
    AtrProbe probe{ ctx };
    EXPECT_DOUBLE_EQ(0.5, probe.customParam("eth_atr_step", 0.5));
}

TEST(UnifiedScalper, CooldownDisabledByFlag)
{
    // Mirrors MomentumScalper: cooldownEnabled() = false → no cooldown gate
    // even though cooldown_seconds is non-zero.
    FeedHarness harness{ MarketType::Futures };
    auto scalper = std::make_unique<TestScalper>(make_ctx(harness, "BTC_USDT"));
    scalper->cooldown_flag = false;
    scalper->params().min_confidence.store(0.0, std::memory_order_release);
    scalper->params().cooldown_seconds.store(60.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    auto candles = make_candles(2);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[0]);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[1]);

    market::Kline trigger;
    trigger.closed = true;
    scalper->onKline(trigger);
    scalper->onKline(trigger);

    EXPECT_EQ(2u, received.size());
}
