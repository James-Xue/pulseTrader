// test_market_feed_stats.cpp — Unit tests for MarketFeed event counters (Layer 3)

#include "market/market_feed.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
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
// FeedStats delta computation — the pattern used by log_system_heartbeat
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
