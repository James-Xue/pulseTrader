// test_warmup_seeder.cpp — WarmupSeeder unit tests (M30 startup preload)

#include "backtest/WarmupSeeder.hpp"

#include "strategy/scalping/EmaResonanceScalper.hpp"

#include "exchange/GateRestClient.hpp"
#include "market/MarketFeed.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using namespace pulse;
using namespace pulse::backtest;
using namespace pulse::strategy;

namespace pulse::backtest::test
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

market::Kline makeCandle(std::int64_t open_ms, double close = 100.0,
                         bool closed = true)
{
    market::Kline k;
    k.open_time = open_ms;
    k.close_time = open_ms + 60'000;
    k.open = close;
    k.high = close + 1.0;
    k.low = close - 1.0;
    k.close = close;
    k.volume = 10.0;
    k.closed = closed;
    return k;
}

/// Stub API source: serves fixed candles in the requested range (the loader
/// paginates by calling fetch per gap; one call covers the whole window).
class SeederApiSource final : public IKlineSource
{
  public:
    std::vector<market::Kline> candles_to_serve;

    Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) override
    {
        (void)symbol;
        (void)market_type;
        std::vector<market::Kline> out;
        for (const auto &c : candles_to_serve)
        {
            if (from_ms <= c.open_time && c.open_time <= to_ms)
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string description() const override
    {
        return "stub-api";
    }
};

/// Stub local source that fails — models a missing/empty sqlite DB so the
/// seeder degrades to pure API.
class SeederLocalSource final : public IKlineSource
{
  public:
    Result<std::vector<market::Kline>> fetch(
        const std::string &, MarketType, std::int64_t, std::int64_t) override
    {
        return PulseError{ ErrorCode::BacktestSqliteUnavailable, "stub local failure" };
    }

    std::string description() const override
    {
        return "stub-failing-local";
    }
};

// FeedHarness — MarketFeed constructible without network I/O (same as the
// strategy tests: nullptr WS client, feed never started).
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

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(WarmupSeeder, SeedsClosedCandlesIntoBuffer)
{
    FeedHarness harness{ MarketType::Futures };
    SeederApiSource api;
    SeederLocalSource local; // no sqlite → pure API.

    // 500 trailing candles, the oldest 60s behind "now", ascending.
    const auto now = now_ms();
    for (std::size_t i = 0; i < 500; ++i)
    {
        api.candles_to_serve.push_back(
            makeCandle(now - static_cast<std::int64_t>(500 - i) * 60'000, 100.0 + i));
    }

    WarmupSeeder seeder(&local, api);
    const auto seeded = seeder.seed(harness.feed, { "ETH_USDT" }, MarketType::Futures);

    EXPECT_EQ(500u, seeded);
    auto snapshot = harness.feed.getKlineBuffer("ETH_USDT").snapshot(500);
    EXPECT_EQ(500u, snapshot.size());
    // Ascending open_time and all closed (the strategy loop gates on closed).
    for (std::size_t i = 1; i < snapshot.size(); ++i)
    {
        EXPECT_LT(snapshot[i - 1].open_time, snapshot[i].open_time);
        EXPECT_TRUE(snapshot[i - 1].closed);
    }
    EXPECT_TRUE(snapshot.back().closed);
}

TEST(WarmupSeeder, SkipsFormingCandle)
{
    FeedHarness harness{ MarketType::Futures };
    SeederApiSource api;
    SeederLocalSource local;

    const auto now = now_ms();
    api.candles_to_serve.push_back(makeCandle(now - 60'000, 100.0));      // closed
    api.candles_to_serve.push_back(makeCandle(now, 100.0, /*closed=*/false)); // forming

    WarmupSeeder seeder(&local, api);
    const auto seeded = seeder.seed(harness.feed, { "ETH_USDT" }, MarketType::Futures);

    EXPECT_EQ(1u, seeded);
    auto snapshot = harness.feed.getKlineBuffer("ETH_USDT").snapshot(2);
    ASSERT_EQ(1u, snapshot.size());
    EXPECT_EQ(now - 60'000, snapshot[0].open_time);
}

TEST(WarmupSeeder, FailureDegradesToNoOp)
{
    FeedHarness harness{ MarketType::Futures };
    SeederApiSource api;               // empty — fetch returns an empty vector.
    SeederLocalSource local;

    WarmupSeeder seeder(&local, api);
    const auto seeded = seeder.seed(harness.feed, { "ETH_USDT" }, MarketType::Futures);

    EXPECT_EQ(0u, seeded);
    EXPECT_TRUE(harness.feed.getKlineBuffer("ETH_USDT").snapshot(1).empty());
}

TEST(WarmupSeeder, SeededBufferWarmsUpStrategy)
{
    // Inject exactly 201 closed candles (EmaResonance warmupThreshold) via
    // the seeder, then one more push must produce an evaluation + signal —
    // i.e. the strategy loop's first poll completes warmup instantly.
    FeedHarness harness{ MarketType::Futures };
    SeederApiSource api;
    SeederLocalSource local;

    const auto now = now_ms();
    for (std::size_t i = 0; i < 201; ++i)
    {
        api.candles_to_serve.push_back(
            makeCandle(now - static_cast<std::int64_t>(201 - i) * 60'000, 100.0));
    }

    WarmupSeeder seeder(&local, api);
    EXPECT_EQ(201u, seeder.seed(harness.feed, { "ETH_USDT" }, MarketType::Futures));

    // Wire a real EmaResonanceScalper to the same feed (params: no cooldown).
    StrategyContext ctx;
    ctx.config.name = "ema_resonance_scalper";
    ctx.config.symbol = "ETH_USDT";
    ctx.market_feed = &harness.feed;

    std::vector<TradingSignal> received;
    auto scalper = std::make_unique<EmaResonanceScalper>(ctx);
    scalper->params().min_confidence.store(0.0, std::memory_order_release);
    scalper->params().cooldown_seconds.store(0.0, std::memory_order_release);
    scalper->setSignalCallback([&received](const TradingSignal &s)
        {
            received.push_back(s);
        });

    // Buffer holds 201 flats → warmup gate passes on the first onKline.
    market::Kline trigger;
    trigger.closed = true;
    scalper->onKline(trigger);

    // First evaluation: all EMAs flat → regime None → no signal yet, but the
    // warmup gate was passed (no "Warming up" stall). Then a bullish candle
    // must produce a Buy via the seeded history.
    market::Kline bull;
    bull.open_time = now + 60'000;
    bull.close_time = now + 120'000;
    bull.open = bull.high = bull.low = bull.close = 102.5;
    bull.closed = true;
    harness.feed.getKlineBuffer("ETH_USDT").push(bull);
    scalper->onKline(bull);

    ASSERT_EQ(1u, received.size());
    EXPECT_EQ(SignalType::Buy, received[0].type);
}

} // namespace pulse::backtest::test
