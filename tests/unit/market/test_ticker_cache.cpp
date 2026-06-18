// test_ticker_cache.cpp — Unit tests for TickerCache (Layer 3 Market Data)

#include "market/ticker_cache.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

using namespace pulse;
using namespace pulse::market;

// ---------------------------------------------------------------------------
// Basic functionality
// ---------------------------------------------------------------------------

TEST(TickerCache, GetReturnsNulloptForUnknownSymbol)
{
    // A fresh cache must return std::nullopt for any symbol.
    TickerCache cache;
    EXPECT_FALSE(cache.get("BTC_USDT").has_value());
}

TEST(TickerCache, UpdateAndRetrieve)
{
    // After update(), get() must return the same ticker data.
    TickerCache cache;

    Ticker ticker;
    ticker.symbol = "BTC_USDT";
    ticker.last = 50000.0;
    ticker.bid = 49999.0;
    ticker.ask = 50001.0;
    ticker.volume_24h = 1234.56;
    ticker.change_pct = 2.5;
    ticker.timestamp = 1234567890000;

    cache.update("BTC_USDT", ticker);

    const auto result = cache.get("BTC_USDT");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol, "BTC_USDT");
    EXPECT_DOUBLE_EQ(result->last, 50000.0);
    EXPECT_DOUBLE_EQ(result->bid, 49999.0);
    EXPECT_DOUBLE_EQ(result->ask, 50001.0);
    EXPECT_DOUBLE_EQ(result->volume_24h, 1234.56);
    EXPECT_DOUBLE_EQ(result->change_pct, 2.5);
    EXPECT_EQ(result->timestamp, 1234567890000);
}

TEST(TickerCache, UpdateOverwritesPrevious)
{
    // A second update() must replace the previous ticker.
    TickerCache cache;

    Ticker ticker1;
    ticker1.symbol = "ETH_USDT";
    ticker1.last = 3000.0;
    cache.update("ETH_USDT", ticker1);

    Ticker ticker2;
    ticker2.symbol = "ETH_USDT";
    ticker2.last = 3100.0;
    cache.update("ETH_USDT", ticker2);

    const auto result = cache.get("ETH_USDT");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->last, 3100.0);
}

TEST(TickerCache, MultipleSymbols)
{
    // Cache must store multiple symbols independently.
    TickerCache cache;

    Ticker btc;
    btc.symbol = "BTC_USDT";
    btc.last = 50000.0;
    cache.update("BTC_USDT", btc);

    Ticker eth;
    eth.symbol = "ETH_USDT";
    eth.last = 3000.0;
    cache.update("ETH_USDT", eth);

    EXPECT_DOUBLE_EQ(cache.get("BTC_USDT")->last, 50000.0);
    EXPECT_DOUBLE_EQ(cache.get("ETH_USDT")->last, 3000.0);
    EXPECT_EQ(cache.size(), 2u);
}

// ---------------------------------------------------------------------------
// Thread safety
// ---------------------------------------------------------------------------

TEST(TickerCache, ConcurrentUpdatesAndReads)
{
    // Multiple threads updating and reading concurrently must not crash or corrupt data.
    TickerCache cache;
    constexpr int kUpdatesPerThread = 1000;
    constexpr int kNumThreads = 4;

    // Writer threads: each updates the same symbol with incrementing prices.
    auto writer = [&cache](int thread_id)
    {
        for (int i = 0; i < kUpdatesPerThread; ++i)
        {
            Ticker ticker;
            ticker.symbol = "BTC_USDT";
            ticker.last = static_cast<double>(thread_id * kUpdatesPerThread + i);
            cache.update("BTC_USDT", ticker);
        }
    };

    // Reader threads: repeatedly read the symbol (may see any value).
    std::atomic<bool> stop{ false };
    auto reader = [&cache, &stop]()
    {
        while (!stop.load())
        {
            const auto opt = cache.get("BTC_USDT");
            // Value may or may not be present; just ensure no crash.
            (void)opt;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kNumThreads; ++i)
    {
        threads.emplace_back(writer, i);
    }
    for (int i = 0; i < 2; ++i)
    {
        threads.emplace_back(reader);
    }

    // Wait for writers to finish.
    for (int i = 0; i < kNumThreads; ++i)
    {
        threads[i].join();
    }

    // Stop readers.
    stop.store(true);
    for (int i = kNumThreads; i < kNumThreads + 2; ++i)
    {
        threads[i].join();
    }

    // Final read must succeed (no crash, value is one of the written values).
    const auto result = cache.get("BTC_USDT");
    ASSERT_TRUE(result.has_value());
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(TickerCache, ContainsReturnsTrueAfterUpdate)
{
    TickerCache cache;
    EXPECT_FALSE(cache.contains("BTC_USDT"));

    Ticker ticker;
    ticker.symbol = "BTC_USDT";
    cache.update("BTC_USDT", ticker);

    EXPECT_TRUE(cache.contains("BTC_USDT"));
    EXPECT_FALSE(cache.contains("ETH_USDT"));
}

TEST(TickerCache, SizeReflectsUniqueSymbols)
{
    TickerCache cache;
    EXPECT_EQ(cache.size(), 0u);

    Ticker btc;
    btc.symbol = "BTC_USDT";
    cache.update("BTC_USDT", btc);
    EXPECT_EQ(cache.size(), 1u);

    // Update same symbol again — size unchanged.
    cache.update("BTC_USDT", btc);
    EXPECT_EQ(cache.size(), 1u);

    // Add new symbol — size increases.
    Ticker eth;
    eth.symbol = "ETH_USDT";
    cache.update("ETH_USDT", eth);
    EXPECT_EQ(cache.size(), 2u);
}

// ---------------------------------------------------------------------------
// symbols() — interface gap bridge for dashboard
// ---------------------------------------------------------------------------

TEST(TickerCache, SymbolsReturnsEmptyVectorWhenCacheIsEmpty)
{
    // A fresh cache must return an empty vector from symbols().
    TickerCache cache;
    const auto result = cache.symbols();
    EXPECT_TRUE(result.empty());
}

TEST(TickerCache, SymbolsReturnsAllCachedSymbols)
{
    // After populating the cache, symbols() must return all stored symbols.
    TickerCache cache;

    Ticker btc;
    btc.symbol = "BTC_USDT";
    btc.last = 50000.0;
    cache.update("BTC_USDT", btc);

    Ticker eth;
    eth.symbol = "ETH_USDT";
    eth.last = 3000.0;
    cache.update("ETH_USDT", eth);

    Ticker sol;
    sol.symbol = "SOL_USDT";
    sol.last = 150.0;
    cache.update("SOL_USDT", sol);

    const auto result = cache.symbols();
    ASSERT_EQ(result.size(), 3u);

    // Order is unspecified (unordered_map), so sort before comparing.
    std::vector<std::string> sorted_result(result.begin(), result.end());
    std::sort(sorted_result.begin(), sorted_result.end());

    EXPECT_EQ(sorted_result[0], "BTC_USDT");
    EXPECT_EQ(sorted_result[1], "ETH_USDT");
    EXPECT_EQ(sorted_result[2], "SOL_USDT");
}

TEST(TickerCache, SymbolsDoesNotDuplicateAfterOverwrite)
{
    // Updating an existing symbol must not create a duplicate in symbols().
    TickerCache cache;

    Ticker btc1;
    btc1.symbol = "BTC_USDT";
    btc1.last = 50000.0;
    cache.update("BTC_USDT", btc1);

    Ticker btc2;
    btc2.symbol = "BTC_USDT";
    btc2.last = 51000.0;
    cache.update("BTC_USDT", btc2);

    const auto result = cache.symbols();
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "BTC_USDT");
}
