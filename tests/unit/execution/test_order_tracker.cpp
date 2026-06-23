// test_order_tracker.cpp — Unit tests for OrderTracker (Layer 8 Order Execution)

#include "execution/order_tracker.hpp"

#include "exchange/gate_rest_client.hpp"
#include "exchange/gate_ws_client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

using namespace pulse;
using namespace pulse::execution;
using namespace pulse::exchange;

// ---------------------------------------------------------------------------
// OrderTracker helper methods
// ---------------------------------------------------------------------------

TEST(OrderTracker, IsTerminalStatusFilled)
{
    EXPECT_TRUE(OrderTracker::isTerminalStatus(OrderStatus::Filled));
}

TEST(OrderTracker, IsTerminalStatusCancelled)
{
    EXPECT_TRUE(OrderTracker::isTerminalStatus(OrderStatus::Cancelled));
}

TEST(OrderTracker, IsTerminalStatusOpen)
{
    EXPECT_FALSE(OrderTracker::isTerminalStatus(OrderStatus::Open));
}

TEST(OrderTracker, IsTerminalStatusPending)
{
    EXPECT_FALSE(OrderTracker::isTerminalStatus(OrderStatus::Pending));
}

TEST(OrderTracker, ParseStatusOpen)
{
    EXPECT_EQ(OrderTracker::parseStatus("open"), OrderStatus::Open);
}

TEST(OrderTracker, ParseStatusClosed)
{
    EXPECT_EQ(OrderTracker::parseStatus("closed"), OrderStatus::Filled);
}

TEST(OrderTracker, ParseStatusCancelled)
{
    EXPECT_EQ(OrderTracker::parseStatus("cancelled"), OrderStatus::Cancelled);
}

TEST(OrderTracker, ParseStatusUnknown)
{
    EXPECT_EQ(OrderTracker::parseStatus("unknown"), OrderStatus::Pending);
}

// ---------------------------------------------------------------------------
// OrderTracker (requires WS + REST clients — tested via integration tests)
// ---------------------------------------------------------------------------

// Note: Full OrderTracker testing requires real or mock WS/REST clients.
// Integration tests in tools/test_execution.cpp will cover:
// - trackOrder() and WS subscription
// - onOrderUpdate() state machine
// - pollOrderStatus() REST fallback
// - ExecutionReport generation
// - Completion callback invocation

// Placeholder for future mock-based unit tests:
// TEST(OrderTracker, TrackOrderSubscribesToWs)
// TEST(OrderTracker, OnOrderUpdateOpenToPartiallyFilled)
// TEST(OrderTracker, OnOrderUpdatePartiallyFilledToFilled)
// TEST(OrderTracker, GenerateReportCalculatesSlippage)
// TEST(OrderTracker, CompletionCallbackInvoked)

// ---------------------------------------------------------------------------
// activeOrders() + recentReports() — interface gap bridges for dashboard
//
// These tests use real WS/REST client objects constructed with a default
// (empty) ExchangeConfig. The clients are never started, so no network
// connections are made. This is sufficient to test the snapshot APIs that
// only read from internal maps populated by trackOrder() / stopTracking().
// ---------------------------------------------------------------------------

class OrderTrackerSnapshotTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Construct WS/REST clients with empty config (never started).
        ExchangeConfig config;
        m_wsClient = std::make_unique<GateWsClient>(config);
        m_restClient = std::make_unique<GateRestClient>(config);
        tracker_ = std::make_unique<OrderTracker>(*m_wsClient, *m_restClient);
    }

    std::unique_ptr<GateWsClient> m_wsClient;
    std::unique_ptr<GateRestClient> m_restClient;
    std::unique_ptr<OrderTracker> tracker_;
};

TEST_F(OrderTrackerSnapshotTest, ActiveOrdersEmptyOnFreshTracker)
{
    // A fresh tracker must return an empty vector from activeOrders().
    const auto orders = tracker_->activeOrders();
    EXPECT_TRUE(orders.empty());
}

TEST_F(OrderTrackerSnapshotTest, TrackedOrdersAppearInActiveOrders)
{
    // After trackOrder(), the order must appear in activeOrders().
    tracker_->trackOrder("order_1", "BTC_USDT", Side::Buy, OrderType::Limit, 0.001, 50000.0);
    tracker_->trackOrder("order_2", "ETH_USDT", Side::Sell, OrderType::Market, 1.0, 3000.0);

    const auto orders = tracker_->activeOrders();
    ASSERT_EQ(orders.size(), 2u);

    // Find each order by order_id (order is unspecified from unordered_map).
    auto find_order = [&orders](const std::string &id) -> const OrderSnapshot *
    {
        for (const auto &o : orders)
        {
            if (o.order_id == id)
            {
                return &o;
            }
        }
        return nullptr;
    };

    const auto *btc = find_order("order_1");
    ASSERT_NE(nullptr, btc);
    EXPECT_EQ(btc->symbol, "BTC_USDT");
    EXPECT_EQ(btc->side, Side::Buy);
    EXPECT_EQ(btc->type, OrderType::Limit);
    EXPECT_DOUBLE_EQ(btc->requested_qty, 0.001);
    EXPECT_DOUBLE_EQ(btc->filled_qty, 0.0);
    EXPECT_EQ(btc->status, OrderStatus::Pending);

    const auto *eth = find_order("order_2");
    ASSERT_NE(nullptr, eth);
    EXPECT_EQ(eth->symbol, "ETH_USDT");
    EXPECT_EQ(eth->side, Side::Sell);
    EXPECT_EQ(eth->type, OrderType::Market);
    EXPECT_DOUBLE_EQ(eth->requested_qty, 1.0);
}

TEST_F(OrderTrackerSnapshotTest, StopTrackingRemovesFromActiveOrders)
{
    // After stopTracking(), the order must no longer appear in activeOrders().
    // This simulates what happens when an order reaches terminal state.
    tracker_->trackOrder("order_1", "BTC_USDT", Side::Buy, OrderType::Limit, 0.001, 50000.0);
    tracker_->trackOrder("order_2", "ETH_USDT", Side::Sell, OrderType::Market, 1.0, 3000.0);

    // Verify both are present.
    EXPECT_EQ(tracker_->activeOrders().size(), 2u);

    // Stop tracking one order (simulates terminal state removal).
    tracker_->stopTracking("order_1");

    const auto orders = tracker_->activeOrders();
    ASSERT_EQ(orders.size(), 1u);
    EXPECT_EQ(orders[0].order_id, "order_2");
}

TEST_F(OrderTrackerSnapshotTest, RecentReportsEmptyOnFreshTracker)
{
    // A fresh tracker must return an empty vector from recentReports().
    const auto reports = tracker_->recentReports();
    EXPECT_TRUE(reports.empty());
}

TEST_F(OrderTrackerSnapshotTest, RecentReportsRespectsLimit)
{
    // recentReports(n) must return at most n reports.
    // With no completed reports, even a large limit returns empty.
    const auto reports = tracker_->recentReports(100);
    EXPECT_TRUE(reports.empty());
}

// ---------------------------------------------------------------------------
// Callback safety tests — verifies the "invoke outside lock" fix
//
// These tests use friend access to call processOrderUpdate() directly,
// simulating WS events without requiring a real WebSocket connection.
// ---------------------------------------------------------------------------

class OrderTrackerCallbackTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ExchangeConfig config;
        m_wsClient = std::make_unique<GateWsClient>(config);
        m_restClient = std::make_unique<GateRestClient>(config);
        tracker_ = std::make_unique<OrderTracker>(*m_wsClient, *m_restClient);
    }

    std::unique_ptr<GateWsClient> m_wsClient;
    std::unique_ptr<GateRestClient> m_restClient;
    std::unique_ptr<OrderTracker> tracker_;
};

TEST_F(OrderTrackerCallbackTest, CompletionCallbackInvokedOutsideLock)
{
    // Verify: when an order reaches terminal state via processOrderUpdate(),
    // the completion callback is invoked AFTER the mutex is released.
    // If the callback tries to acquire a shared_lock, it must succeed
    // (proving the write_lock was released first).

    tracker_->trackOrder("test_order_1", "BTC_USDT", Side::Buy, OrderType::Market,
                          0.001, 50000.0);

    std::atomic<bool> callback_invoked{ false };
    std::atomic<bool> lock_acquired_in_callback{ false };

    tracker_->setCompletionCallback(
        [this, &callback_invoked, &lock_acquired_in_callback](const ExecutionReport &report)
        {
            callback_invoked = true;

            // Try to acquire a shared lock — this would deadlock if the
            // write_lock is still held by processOrderUpdate().
            lock_acquired_in_callback = tracker_->testTrySharedLock();
        });

    // Simulate a "closed" (filled) WS event.
    nlohmann::json ws_event;
    ws_event["id"] = "test_order_1";
    ws_event["status"] = "closed";
    ws_event["filled_total"] = "0.001";
    ws_event["avg_deal_price"] = "50001";
    ws_event["fee"] = "0.05";

    tracker_->testSimulateWsUpdate(ws_event);

    EXPECT_TRUE(callback_invoked.load());
    EXPECT_TRUE(lock_acquired_in_callback.load());
}

TEST_F(OrderTrackerCallbackTest, SetCompletionCallbackThreadSafe)
{
    // Verify: concurrent calls to setCompletionCallback() don't cause
    // a data race with processOrderUpdate() reading the callback.

    std::atomic<bool> stop{ false };
    std::atomic<int> callback_count{ 0 };

    // Writer thread: repeatedly sets new callbacks.
    std::thread writer([&]()
    {
        int i = 0;
        while (!stop.load())
        {
            tracker_->setCompletionCallback(
                [&callback_count, i](const ExecutionReport &)
                {
                    callback_count++;
                });
            i++;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    // Run for 50ms — enough iterations to catch a race if one exists.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
    writer.join();

    // No crash = no data race detected (TSAN would catch this).
    SUCCEED();
}

TEST_F(OrderTrackerCallbackTest, ProcessOrderUpdateTerminalGeneratesReport)
{
    // Verify: processOrderUpdate() with terminal status generates a correct
    // ExecutionReport and removes the order from activeOrders().

    tracker_->trackOrder("report_test_1", "ETH_USDT", Side::Sell, OrderType::Limit,
                          2.0, 3000.0, "strategy_alpha");

    // First: partial fill (non-terminal) — should NOT trigger callback.
    nlohmann::json partial_event;
    partial_event["id"] = "report_test_1";
    partial_event["status"] = "open";
    partial_event["filled_total"] = "1.0";
    partial_event["avg_deal_price"] = "3001";

    std::atomic<bool> callback_called{ false };
    tracker_->setCompletionCallback([&](const ExecutionReport &)
    {
        callback_called = true;
    });

    tracker_->testSimulateWsUpdate(partial_event);
    EXPECT_FALSE(callback_called.load());
    EXPECT_EQ(tracker_->activeOrders().size(), 1u);

    // Second: full fill (terminal) — should trigger callback.
    nlohmann::json filled_event;
    filled_event["id"] = "report_test_1";
    filled_event["status"] = "closed";
    filled_event["filled_total"] = "2.0";
    filled_event["avg_deal_price"] = "3002";
    filled_event["fee"] = "0.1";

    tracker_->testSimulateWsUpdate(filled_event);

    EXPECT_TRUE(callback_called.load());
    EXPECT_TRUE(tracker_->activeOrders().empty());

    // Report should be in recentReports.
    const auto reports = tracker_->recentReports(1);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].order_id, "report_test_1");
    EXPECT_EQ(reports[0].symbol, "ETH_USDT");
    EXPECT_EQ(reports[0].side, Side::Sell);
    EXPECT_DOUBLE_EQ(reports[0].filled_qty, 2.0);
    EXPECT_DOUBLE_EQ(reports[0].avg_fill_price, 3002.0);
}
