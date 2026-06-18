// test_orderbook_manager.cpp — Unit tests for OrderBookManager (Layer 3 Market Data)

#include "market/orderbook_manager.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::market;

// ---------------------------------------------------------------------------
// Snapshot application
// ---------------------------------------------------------------------------

TEST(OrderBookManager, ApplySnapshot)
{
    OrderBookManager manager;

    const nlohmann::json snapshot = {
        { "lastUpdateId", 100 },
        { "time", 1234567890000 },
        { "bids",
            nlohmann::json::array(
                { nlohmann::json::array({ 50000.0, 1.5 }), nlohmann::json::array({ 49999.0, 2.0 }) }) },
        { "asks",
            nlohmann::json::array(
                { nlohmann::json::array({ 50001.0, 1.0 }), nlohmann::json::array({ 50002.0, 0.5 }) }) }
    };

    manager.apply_snapshot("BTC_USDT", snapshot);

    const auto book = manager.get("BTC_USDT");
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->sequence_id, 100u);
    EXPECT_EQ(book->bids.size(), 2u);
    EXPECT_EQ(book->asks.size(), 2u);
}

TEST(OrderBookManager, SnapshotBidsSortedDescending)
{
    OrderBookManager manager;

    const nlohmann::json snapshot = {
        { "lastUpdateId", 1 },
        { "bids",
            nlohmann::json::array({ nlohmann::json::array({ 100.0, 1.0 }),
                nlohmann::json::array({ 102.0, 1.0 }),
                nlohmann::json::array({ 101.0, 1.0 }) }) },
        { "asks", nlohmann::json::array() }
    };

    manager.apply_snapshot("TEST_USDT", snapshot);

    const auto top = manager.top_bids("TEST_USDT", 3);
    ASSERT_EQ(top.size(), 3u);
    EXPECT_DOUBLE_EQ(top[0].price, 102.0);
    EXPECT_DOUBLE_EQ(top[1].price, 101.0);
    EXPECT_DOUBLE_EQ(top[2].price, 100.0);
}

TEST(OrderBookManager, SnapshotAsksSortedAscending)
{
    OrderBookManager manager;

    const nlohmann::json snapshot = {
        { "lastUpdateId", 1 },
        { "bids", nlohmann::json::array() },
        { "asks",
            nlohmann::json::array({ nlohmann::json::array({ 102.0, 1.0 }),
                nlohmann::json::array({ 100.0, 1.0 }),
                nlohmann::json::array({ 101.0, 1.0 }) }) }
    };

    manager.apply_snapshot("TEST_USDT", snapshot);

    const auto top = manager.top_asks("TEST_USDT", 3);
    ASSERT_EQ(top.size(), 3u);
    EXPECT_DOUBLE_EQ(top[0].price, 100.0);
    EXPECT_DOUBLE_EQ(top[1].price, 101.0);
    EXPECT_DOUBLE_EQ(top[2].price, 102.0);
}

// ---------------------------------------------------------------------------
// Delta application
// ---------------------------------------------------------------------------

TEST(OrderBookManager, ApplyDeltaUpdatesLevels)
{
    OrderBookManager manager;

    const nlohmann::json snapshot = {
        { "lastUpdateId", 100 },
        { "bids", nlohmann::json::array({ nlohmann::json::array({ 100.0, 1.0 }) }) },
        { "asks", nlohmann::json::array({ nlohmann::json::array({ 101.0, 1.0 }) }) }
    };
    manager.apply_snapshot("TEST_USDT", snapshot);

    const nlohmann::json delta = {
        { "lastUpdateId", 101 },
        { "bids", nlohmann::json::array({ nlohmann::json::array({ 100.0, 2.0 }) }) }, // Update qty.
        { "asks", nlohmann::json::array({ nlohmann::json::array({ 102.0, 1.5 }) }) }  // Add new level.
    };
    manager.apply_delta("TEST_USDT", delta);

    const auto book = manager.get("TEST_USDT");
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->sequence_id, 101u);
    EXPECT_DOUBLE_EQ(book->bids.at(100.0), 2.0);
    EXPECT_DOUBLE_EQ(book->asks.at(102.0), 1.5);
}

TEST(OrderBookManager, ApplyDeltaRemovesZeroQuantityLevels)
{
    OrderBookManager manager;

    const nlohmann::json snapshot = {
        { "lastUpdateId", 100 },
        { "bids", nlohmann::json::array({ nlohmann::json::array({ 100.0, 1.0 }) }) },
        { "asks", nlohmann::json::array({ nlohmann::json::array({ 101.0, 1.0 }) }) }
    };
    manager.apply_snapshot("TEST_USDT", snapshot);

    const nlohmann::json delta = {
        { "lastUpdateId", 101 },
        { "bids", nlohmann::json::array({ nlohmann::json::array({ 100.0, 0.0 }) }) }, // Remove.
        { "asks", nlohmann::json::array() }
    };
    manager.apply_delta("TEST_USDT", delta);

    const auto book = manager.get("TEST_USDT");
    ASSERT_TRUE(book.has_value());
    EXPECT_TRUE(book->bids.empty());
}

// ---------------------------------------------------------------------------
// Sequence validation
// ---------------------------------------------------------------------------

TEST(OrderBookManager, SequenceGapTriggersResubscribe)
{
    OrderBookManager manager;
    std::string resubscribed_symbol;
    manager.set_resubscribe_callback([&resubscribed_symbol](const Symbol &s) { resubscribed_symbol = s; });

    const nlohmann::json snapshot = {
        { "lastUpdateId", 100 },
        { "bids", nlohmann::json::array() },
        { "asks", nlohmann::json::array() }
    };
    manager.apply_snapshot("TEST_USDT", snapshot);

    // Delta with gap: expected 101, got 105.
    const nlohmann::json delta = {
        { "lastUpdateId", 105 },
        { "bids", nlohmann::json::array() },
        { "asks", nlohmann::json::array() }
    };
    manager.apply_delta("TEST_USDT", delta);

    EXPECT_EQ(resubscribed_symbol, "TEST_USDT");
}

TEST(OrderBookManager, DeltaBeforeSnapshotIsIgnored)
{
    OrderBookManager manager;

    const nlohmann::json delta = {
        { "lastUpdateId", 101 },
        { "bids", nlohmann::json::array() },
        { "asks", nlohmann::json::array() }
    };
    manager.apply_delta("TEST_USDT", delta);

    EXPECT_FALSE(manager.contains("TEST_USDT"));
}

// ---------------------------------------------------------------------------
// Top N bids/asks
// ---------------------------------------------------------------------------

TEST(OrderBookManager, TopNBids)
{
    OrderBookManager manager;

    const nlohmann::json snapshot = {
        { "lastUpdateId", 1 },
        { "bids",
            nlohmann::json::array({ nlohmann::json::array({ 100.0, 1.0 }),
                nlohmann::json::array({ 99.0, 2.0 }),
                nlohmann::json::array({ 98.0, 3.0 }),
                nlohmann::json::array({ 97.0, 4.0 }) }) },
        { "asks", nlohmann::json::array() }
    };
    manager.apply_snapshot("TEST_USDT", snapshot);

    const auto top2 = manager.top_bids("TEST_USDT", 2);
    ASSERT_EQ(top2.size(), 2u);
    EXPECT_DOUBLE_EQ(top2[0].price, 100.0);
    EXPECT_DOUBLE_EQ(top2[1].price, 99.0);
}

TEST(OrderBookManager, TopNAsks)
{
    OrderBookManager manager;

    const nlohmann::json snapshot = {
        { "lastUpdateId", 1 },
        { "bids", nlohmann::json::array() },
        { "asks",
            nlohmann::json::array({ nlohmann::json::array({ 100.0, 1.0 }),
                nlohmann::json::array({ 101.0, 2.0 }),
                nlohmann::json::array({ 102.0, 3.0 }) }) }
    };
    manager.apply_snapshot("TEST_USDT", snapshot);

    const auto top5 = manager.top_asks("TEST_USDT", 5);
    ASSERT_EQ(top5.size(), 3u); // Only 3 available.
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(OrderBookManager, GetReturnsNulloptForUnknownSymbol)
{
    OrderBookManager manager;
    EXPECT_FALSE(manager.get("UNKNOWN_USDT").has_value());
}

TEST(OrderBookManager, ContainsReturnsFalseInitially)
{
    OrderBookManager manager;
    EXPECT_FALSE(manager.contains("BTC_USDT"));
}
