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

#include <cstdint>
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

// Like make_candles but with explicit 1m open/close times starting at
// `start_ms` (the news gate judges candle open_time — untimed candles
// (open_time == 0) would never fall in a 2026-era window).
static std::vector<market::Kline> make_timed_candles(std::size_t count,
    std::int64_t start_ms, double close_step = 1.0)
{
    auto candles = make_candles(count, close_step);
    for (std::size_t i = 0; i < candles.size(); ++i)
    {
        candles[i].open_time = start_ms + static_cast<std::int64_t>(i) * 60'000;
        candles[i].close_time = candles[i].open_time + 60'000;
    }
    return candles;
}

// FlatScalper — emits a Flat (close-only) entry per evaluation; proves the
// base net always lets the flatten/exit channel through a blackout.
class FlatScalper : public TestScalper
{
  public:
    using TestScalper::TestScalper;

  protected:
    std::optional<EntryContext> evaluateEntry(
        const std::vector<market::Kline> & /*candles*/) override
    {
        ++eval_calls;
        EntryContext e;
        e.type = SignalType::Flat;
        e.confidence = 0.0;
        e.reason = "test flat";
        return e;
    }
};

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

TEST(UnifiedScalper, NewsGateDropsEntrySignalsWhileBlocked)
{
    FeedHarness harness{ MarketType::Futures };
    const std::int64_t kEvent = 1'800'000'000'000LL;   // fixed event instant
    NewsWindow w;
    w.event_open_ms = kEvent;
    w.close_before_min = 15.0;
    w.resume_after_min = 15.0;   // blackout [kEvent−15m, kEvent+15m)

    auto ctx = make_ctx(harness, "BTC_USDT");
    ctx.config.news_windows.push_back(w);

    auto scalper = std::make_unique<TestScalper>(ctx);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // Two candles whose LAST open_time is inside the blackout → the entry
    // signal is dropped here (after evaluateEntry already ran — the same
    // "state committed, signal dropped" contract as the cooldown gate).
    auto candles = make_timed_candles(2, kEvent - 10 * 60'000);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[0]);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[1]);

    market::Kline trigger;
    trigger.closed = true;
    scalper->onKline(trigger);
    EXPECT_EQ(1u, scalper->eval_calls);   // evaluateEntry still ran
    EXPECT_TRUE(received.empty());        // ... but the Buy was suppressed

    // Candle open_time at/after T+Y → the entry flows again.
    auto after = make_timed_candles(1, kEvent + 15 * 60'000);
    harness.feed.getKlineBuffer("BTC_USDT").push(after.front());
    scalper->onKline(trigger);
    EXPECT_EQ(2u, scalper->eval_calls);
    ASSERT_EQ(1u, received.size());
    EXPECT_EQ(SignalType::Buy, received.front().type);
}

TEST(UnifiedScalper, NewsGatePassesFlatSignals)
{
    FeedHarness harness{ MarketType::Futures };
    const std::int64_t kEvent = 1'800'000'000'000LL;
    NewsWindow w;
    w.event_open_ms = kEvent;
    w.close_before_min = 15.0;
    w.resume_after_min = 15.0;

    auto ctx = make_ctx(harness, "BTC_USDT");
    ctx.config.news_windows.push_back(w);

    auto scalper = std::make_unique<FlatScalper>(ctx);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    scalper->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // A Flat inside the blackout MUST be delivered — the close-only exit
    // channel stays open so gate flattening and rule exits always flow.
    auto candles = make_timed_candles(2, kEvent - 5 * 60'000);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[0]);
    harness.feed.getKlineBuffer("BTC_USDT").push(candles[1]);

    market::Kline trigger;
    trigger.closed = true;
    scalper->onKline(trigger);
    EXPECT_EQ(1u, scalper->eval_calls);
    ASSERT_EQ(1u, received.size());
    EXPECT_EQ(SignalType::Flat, received.front().type);
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
