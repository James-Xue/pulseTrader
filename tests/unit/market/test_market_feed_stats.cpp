// test_market_feed_stats.cpp — Unit tests for MarketFeed event counters (Layer 3)

#include "market/MarketFeed.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace pulse;
using namespace pulse::market;

// ---------------------------------------------------------------------------
// FeedStats struct — initialization contract
// ---------------------------------------------------------------------------

TEST(MarketFeedStats, DefaultInitialization)
{
    // A value-initialized FeedStats must have all counters at zero.
    FeedStats stats{};
    EXPECT_EQ(0u, stats.ticker_count);
    EXPECT_EQ(0u, stats.orderbook_count);
    EXPECT_EQ(0u, stats.kline_count);
}

TEST(MarketFeedStats, AggregateInitialization)
{
    // FeedStats supports aggregate initialization with designated initializers.
    FeedStats stats{ .ticker_count = 100, .orderbook_count = 50, .kline_count = 25 };
    EXPECT_EQ(100u, stats.ticker_count);
    EXPECT_EQ(50u, stats.orderbook_count);
    EXPECT_EQ(25u, stats.kline_count);
}

// ---------------------------------------------------------------------------
// Atomic counter pattern — thread-safety under concurrent increment
// ---------------------------------------------------------------------------

TEST(MarketFeedStats, ConcurrentIncrement)
{
    // Simulate the same access pattern as MarketFeed callbacks:
    // multiple threads incrementing a relaxed atomic counter.
    // This validates the counter pattern itself (lock xadd on x86).
    std::atomic<std::uint64_t> counter{ 0 };

    constexpr int kThreadCount         = 4;
    constexpr int kIncrementsPerThread = 10000;

    std::vector<std::jthread> threads;
    threads.reserve(kThreadCount);

    for (int i = 0; i < kThreadCount; ++i)
    {
        threads.emplace_back([&counter]()
        {
            for (int j = 0; j < kIncrementsPerThread; ++j)
            {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // jthread destructor joins — all threads complete before we read.
    threads.clear();

    EXPECT_EQ(static_cast<std::uint64_t>(kThreadCount * kIncrementsPerThread),
              counter.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// FeedStats delta computation — the pattern used by logSystemHeartbeat
// ---------------------------------------------------------------------------

TEST(MarketFeedStats, DeltaComputation)
{
    // Verify the delta math used by the heartbeat logger.
    FeedStats prev{ .ticker_count = 1000, .orderbook_count = 500, .kline_count = 100 };
    FeedStats cur { .ticker_count = 7000, .orderbook_count = 5300, .kline_count = 700 };

    const auto tick_delta  = cur.ticker_count    - prev.ticker_count;
    const auto ob_delta    = cur.orderbook_count - prev.orderbook_count;
    const auto kline_delta = cur.kline_count     - prev.kline_count;

    // 6000 ticks, 4800 orderbook updates, 600 klines over 60 seconds.
    EXPECT_EQ(6000u, tick_delta);
    EXPECT_EQ(4800u, ob_delta);
    EXPECT_EQ(600u,  kline_delta);

    // Rate computation (events per second).
    constexpr double kIntervalSec = 60.0;
    EXPECT_DOUBLE_EQ(100.0, tick_delta / kIntervalSec);
    EXPECT_DOUBLE_EQ(80.0,  ob_delta / kIntervalSec);
    EXPECT_DOUBLE_EQ(10.0,  kline_delta / kIntervalSec);
}

TEST(MarketFeedStats, DeltaComputationZeroActivity)
{
    // When no events occur, delta is zero — no division-by-zero or NaN.
    FeedStats prev{ .ticker_count = 500, .orderbook_count = 200, .kline_count = 50 };
    FeedStats cur  = prev; // No change.

    const auto tick_delta  = cur.ticker_count    - prev.ticker_count;
    const auto ob_delta    = cur.orderbook_count - prev.orderbook_count;
    const auto kline_delta = cur.kline_count     - prev.kline_count;

    EXPECT_EQ(0u, tick_delta);
    EXPECT_EQ(0u, ob_delta);
    EXPECT_EQ(0u, kline_delta);

    constexpr double kIntervalSec = 60.0;
    EXPECT_DOUBLE_EQ(0.0, tick_delta / kIntervalSec);
    EXPECT_DOUBLE_EQ(0.0, ob_delta / kIntervalSec);
    EXPECT_DOUBLE_EQ(0.0, kline_delta / kIntervalSec);
}

// ---------------------------------------------------------------------------
// Kline JSON format — symbol extraction differences between spot and futures
//
// These tests validate the JSON structure assumptions used by
// MarketFeed::onKlineUpdate(). The symbol field location differs:
//   Spot:    full_frame["currency_pair"] = "BTC_USDT"
//   Futures: result["n"] = "BTC_USDT"
// ---------------------------------------------------------------------------

TEST(MarketFeedStats, SpotKlineFrameHasCurrencyPairInOuterFrame)
{
    // Spot kline frame: symbol is in the outer frame as "currency_pair".
    // The result object does NOT contain the contract name in "n" — it's the interval.
    const nlohmann::json frame = nlohmann::json::parse(R"({
        "time": 1542162490,
        "channel": "spot.candlesticks",
        "event": "update",
        "result": { "t": 1542162480, "o": "6350.1", "c": "6350.2", "h": "6350.2", "l": "6350.1", "v": "120", "n": "1m", "a": "762012" },
        "currency_pair": "BTC_USDT"
    })");

    const auto &result = frame["result"];

    // Spot: symbol comes from outer frame.
    EXPECT_TRUE(frame.contains("currency_pair"));
    EXPECT_EQ("BTC_USDT", frame["currency_pair"].get<std::string>());

    // In spot, result["n"] is the interval, NOT the contract name.
    EXPECT_TRUE(result.contains("n"));
    EXPECT_EQ("1m", result["n"].get<std::string>());
}

TEST(MarketFeedStats, FuturesKlineFrameHasContractInResult)
{
    // Futures kline frame: symbol is INSIDE result as "n".
    // The outer frame does NOT have "contract" or "currency_pair".
    const nlohmann::json frame = nlohmann::json::parse(R"({
        "time": 1542162490,
        "channel": "futures.candlesticks",
        "event": "update",
        "result": { "t": 1542162480, "o": "6350.1", "c": "6350.2", "h": "6350.2", "l": "6350.1", "v": 120, "n": "BTC_USDT" }
    })");

    const auto &result = frame["result"];

    // Futures: outer frame has NO contract field.
    EXPECT_FALSE(frame.contains("currency_pair"));
    EXPECT_FALSE(frame.contains("contract"));

    // Symbol is in result["n"].
    EXPECT_TRUE(result.contains("n"));
    EXPECT_EQ("BTC_USDT", result["n"].get<std::string>());
}

TEST(MarketFeedStats, FuturesKlineSubscriptionPayloadOrder)
{
    // Gate.io requires payload = ["1m", "BTC_USDT"] (interval first).
    // This test documents the expected payload structure.
    const std::vector<std::string> symbols = { "BTC_USDT", "ETH_USDT" };

    std::vector<std::string> payload;
    payload.push_back("1m");
    for (const auto &s : symbols)
    {
        payload.push_back(s);
    }

    ASSERT_EQ(3u, payload.size());
    EXPECT_EQ("1m",      payload[0]); // Interval first.
    EXPECT_EQ("BTC_USDT", payload[1]);
    EXPECT_EQ("ETH_USDT", payload[2]);
}
