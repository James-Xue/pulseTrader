// test_ema_resonance_scalper.cpp — Unit tests for the five-period EMA
// resonance strategy (full kline pipeline, same harness as test_eth_scalper).
//
// Test data recipe (default periods 7/14/30/60/200; klineNeeded=201):
//   - Call 1: 200 flat candles @100 (high 100.5 / low 99.5, range 1.0)
//     → 200 < 201 warmup gate — evaluateEntry not reached.
//   - Call 2: +1 flat @100 (201 flats) → all EMAs = 100 → regime None,
//     committed; no signal (no transition).
//   - Call 3: +1 candle @102.5 → snapshot = [100×200, 102.5] → fresh
//     full-series EMAs 100.625 > 100.333 > 100.161 > 100.082 > 100.025
//     → None→Bull transition → Buy.
//   - Call 4: +1 @102.5 → regime stays Bull → no repeat signal.
//   - Call 5: +1 @90 → EMAs 98.32 < 99.21 < 99.65 < 99.82 < 99.95
//     → Bull→Bear direct flip → Sell.

#include "strategy/scalping/EmaResonanceScalper.hpp"
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

// Build `count` candles all at `price` (range ±0.5 around close).
static std::vector<market::Kline> make_candles(std::size_t count, double price)
{
    std::vector<market::Kline> candles;
    candles.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        market::Kline k;
        k.open = price;
        k.high = price + 0.5;
        k.low = price - 0.5;
        k.close = price;
        k.closed = true;
        candles.push_back(k);
    }
    return candles;
}

static StrategyContext make_ctx(FeedHarness &harness,
    std::map<std::string, double> custom = {})
{
    StrategyContext ctx;
    ctx.config.name = "ema_resonance_scalper";
    ctx.config.symbol = "ETH_USDT";
    ctx.config.custom_params = std::move(custom);
    ctx.market_feed = &harness.feed;
    return ctx;
}

// Push a batch of candles into the kline buffer, then trigger one onKline
// evaluation (each batch = one evaluation pass).
static void push_candles(FeedHarness &harness, EmaResonanceScalper &scalper,
    const std::vector<market::Kline> &candles)
{
    auto &buf = harness.feed.getKlineBuffer("ETH_USDT");
    for (const auto &c : candles)
    {
        buf.push(c);
    }
    market::Kline trigger;
    trigger.closed = true;
    scalper.onKline(trigger);
}

// Create a configured scalper: min_confidence and cooldown disabled so the
// signal callback is the only gate under test.
static std::unique_ptr<EmaResonanceScalper> make_scalper(FeedHarness &harness,
    std::map<std::string, double> custom = {},
    std::vector<TradingSignal> *out_received = nullptr)
{
    auto scalper = std::make_unique<EmaResonanceScalper>(make_ctx(harness, std::move(custom)));
    scalper->params().min_confidence.store(0.0, std::memory_order_release);
    scalper->params().cooldown_seconds.store(0.0, std::memory_order_release);
    if (nullptr != out_received)
    {
        scalper->setSignalCallback([out_received](const TradingSignal &s)
            {
                out_received->push_back(s);
            });
    }
    return scalper;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(EmaResonanceScalper, NameAndId)
{
    FeedHarness harness{ MarketType::Futures };
    auto scalper = make_scalper(harness);
    EXPECT_EQ("EmaResonanceScalper", scalper->name());
    EXPECT_EQ("ema_resonance_scalper_ETH_USDT", scalper->id());
}

TEST(EmaResonanceScalper, BullishAlignmentEmitsBuy)
{
    FeedHarness harness{ MarketType::Futures };
    std::vector<TradingSignal> received;
    auto scalper = make_scalper(harness, {}, &received);

    push_candles(harness, *scalper, make_candles(200, 100.0)); // warmup
    push_candles(harness, *scalper, make_candles(1, 100.0));   // commit None
    push_candles(harness, *scalper, make_candles(1, 102.5));   // None→Bull

    ASSERT_EQ(1u, received.size());
    const auto &sig = received.back();
    EXPECT_EQ(SignalType::Buy, sig.type);
    EXPECT_EQ("ETH_USDT", sig.symbol);
    EXPECT_EQ("ema_resonance_scalper_ETH_USDT", sig.strategy_id);
    EXPECT_DOUBLE_EQ(102.5, sig.price);
    EXPECT_GT(sig.confidence, 0.0);
    EXPECT_LE(sig.confidence, 1.0);
    EXPECT_NE(std::string::npos, sig.reason.find("bullish"));

    // Indicators: the five EMAs match a fresh full-series recompute with the
    // default periods, and the regime is a strict bull alignment.
    const auto &ind = sig.indicators;
    EXPECT_EQ("bull_aligned", ind.value("resonance", ""));
    std::vector<double> closes(200, 100.0);
    closes.push_back(102.5);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 7.0, 0.0), ind.value("ema7", 0.0), 1e-9);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 14.0, 0.0), ind.value("ema14", 0.0), 1e-9);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 30.0, 0.0), ind.value("ema30", 0.0), 1e-9);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 60.0, 0.0), ind.value("ema60", 0.0), 1e-9);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 200.0, 0.0), ind.value("ema200", 0.0), 1e-9);
    // Strict ordering is the resonance itself.
    const double e1 = ind.value("ema7", 0.0);
    const double e2 = ind.value("ema14", 0.0);
    const double e3 = ind.value("ema30", 0.0);
    const double e4 = ind.value("ema60", 0.0);
    const double e5 = ind.value("ema200", 0.0);
    EXPECT_GT(e1, e2);
    EXPECT_GT(e2, e3);
    EXPECT_GT(e3, e4);
    EXPECT_GT(e4, e5);

    // Confidence: clamp(|ema7 - ema200| / atr × 1.0, 0, 1).
    const double atr = ind.value("atr", 0.0);
    ASSERT_GT(atr, 0.0);
    const double exp_conf = std::clamp(std::abs(e1 - e5) / atr, 0.0, 1.0);
    EXPECT_NEAR(exp_conf, sig.confidence, 1e-9);
}

TEST(EmaResonanceScalper, BearishAlignmentEmitsSell)
{
    FeedHarness harness{ MarketType::Futures };
    std::vector<TradingSignal> received;
    auto scalper = make_scalper(harness, {}, &received);

    push_candles(harness, *scalper, make_candles(200, 100.0)); // warmup
    push_candles(harness, *scalper, make_candles(1, 100.0));   // commit None
    push_candles(harness, *scalper, make_candles(1, 97.5));    // None→Bear

    ASSERT_EQ(1u, received.size());
    const auto &sig = received.back();
    EXPECT_EQ(SignalType::Sell, sig.type);
    EXPECT_EQ("ema_resonance_scalper_ETH_USDT", sig.strategy_id);
    EXPECT_DOUBLE_EQ(97.5, sig.price);
    EXPECT_GT(sig.confidence, 0.0);
    EXPECT_LE(sig.confidence, 1.0);
    EXPECT_NE(std::string::npos, sig.reason.find("bearish"));

    const auto &ind = sig.indicators;
    EXPECT_EQ("bear_aligned", ind.value("resonance", ""));
    const double e1 = ind.value("ema7", 0.0);
    const double e2 = ind.value("ema14", 0.0);
    const double e3 = ind.value("ema30", 0.0);
    const double e4 = ind.value("ema60", 0.0);
    const double e5 = ind.value("ema200", 0.0);
    EXPECT_LT(e1, e2);
    EXPECT_LT(e2, e3);
    EXPECT_LT(e3, e4);
    EXPECT_LT(e4, e5);
}

TEST(EmaResonanceScalper, NoSignalWhenMixed)
{
    // All-flat closes → all EMAs converge to the same value → no strict
    // ordering → regime stays None → no signal across every pass.
    FeedHarness harness{ MarketType::Futures };
    std::vector<TradingSignal> received;
    auto scalper = make_scalper(harness, {}, &received);

    push_candles(harness, *scalper, make_candles(200, 100.0));
    push_candles(harness, *scalper, make_candles(1, 100.0));
    push_candles(harness, *scalper, make_candles(2, 100.0));

    EXPECT_TRUE(received.empty());
}

TEST(EmaResonanceScalper, AlignmentPersistsNoResignalAndDirectFlipSells)
{
    FeedHarness harness{ MarketType::Futures };
    std::vector<TradingSignal> received;
    auto scalper = make_scalper(harness, {}, &received);

    push_candles(harness, *scalper, make_candles(200, 100.0)); // warmup
    push_candles(harness, *scalper, make_candles(1, 100.0));   // commit None
    push_candles(harness, *scalper, make_candles(1, 102.5));   // None→Bull → Buy

    ASSERT_EQ(1u, received.size());
    EXPECT_EQ(SignalType::Buy, received.back().type);

    // Alignment persists (one more candle at the same price): transition is
    // Bull→Bull → NO repeat signal (transition-triggered, not per-candle).
    push_candles(harness, *scalper, make_candles(1, 102.5));
    EXPECT_EQ(1u, received.size());

    // Direct Bull→Bear flip (hard drop) → Sell.
    push_candles(harness, *scalper, make_candles(1, 90.0));
    ASSERT_EQ(2u, received.size());
    EXPECT_EQ(SignalType::Sell, received.back().type);
    EXPECT_EQ("bear_aligned", received.back().indicators.value("resonance", ""));
}

TEST(EmaResonanceScalper, CustomParamsControlPeriods)
{
    // Custom periods (3/5/8/13/21) shorten the stack: klineNeeded becomes
    // 22, so the warmup batch shrinks and the EMA values follow the custom
    // periods instead of the defaults.
    FeedHarness harness{ MarketType::Futures };
    std::vector<TradingSignal> received;
    auto scalper = make_scalper(harness,
        { { "res_ema_p1", 3.0 }, { "res_ema_p2", 5.0 },
          { "res_ema_p3", 8.0 }, { "res_ema_p4", 13.0 },
          { "res_ema_p5", 21.0 }, { "res_conf_scale", 1.0 } },
        &received);

    push_candles(harness, *scalper, make_candles(21, 100.0)); // warmup (< 22)
    push_candles(harness, *scalper, make_candles(1, 100.0));  // commit None
    push_candles(harness, *scalper, make_candles(1, 102.5));  // None→Bull

    ASSERT_EQ(1u, received.size());
    EXPECT_EQ(SignalType::Buy, received.back().type);

    // 23 candles in buffer; snapshot(22) = [100×21, 102.5].
    const auto &ind = received.back().indicators;
    std::vector<double> closes(21, 100.0);
    closes.push_back(102.5);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 3.0, 0.0), ind.value("ema7", 0.0), 1e-9);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 5.0, 0.0), ind.value("ema14", 0.0), 1e-9);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 8.0, 0.0), ind.value("ema30", 0.0), 1e-9);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 13.0, 0.0), ind.value("ema60", 0.0), 1e-9);
    EXPECT_NEAR(EmaProbe::computeEma(closes, 21.0, 0.0), ind.value("ema200", 0.0), 1e-9);
}
