// test_order_tracker.cpp — Unit tests for OrderTracker (Layer 8 Order Execution)

#include "pulse/execution/order_tracker.hpp"

#include "pulse/exchange/gate_rest_client.hpp"
#include "pulse/exchange/gate_ws_client.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace pulse;
using namespace pulse::execution;
using namespace pulse::exchange;

// ---------------------------------------------------------------------------
// OrderTracker helper methods
// ---------------------------------------------------------------------------

TEST(OrderTracker, IsTerminalStatusFilled)
{
    EXPECT_TRUE(OrderTracker::is_terminal_status(OrderStatus::Filled));
}

TEST(OrderTracker, IsTerminalStatusCancelled)
{
    EXPECT_TRUE(OrderTracker::is_terminal_status(OrderStatus::Cancelled));
}

TEST(OrderTracker, IsTerminalStatusOpen)
{
    EXPECT_FALSE(OrderTracker::is_terminal_status(OrderStatus::Open));
}

TEST(OrderTracker, IsTerminalStatusPending)
{
    EXPECT_FALSE(OrderTracker::is_terminal_status(OrderStatus::Pending));
}

TEST(OrderTracker, ParseStatusOpen)
{
    EXPECT_EQ(OrderTracker::parse_status("open"), OrderStatus::Open);
}

TEST(OrderTracker, ParseStatusClosed)
{
    EXPECT_EQ(OrderTracker::parse_status("closed"), OrderStatus::Filled);
}

TEST(OrderTracker, ParseStatusCancelled)
{
    EXPECT_EQ(OrderTracker::parse_status("cancelled"), OrderStatus::Cancelled);
}

TEST(OrderTracker, ParseStatusUnknown)
{
    EXPECT_EQ(OrderTracker::parse_status("unknown"), OrderStatus::Pending);
}

// ---------------------------------------------------------------------------
// OrderTracker (requires WS + REST clients — tested via integration tests)
// ---------------------------------------------------------------------------

// Note: Full OrderTracker testing requires real or mock WS/REST clients.
// Integration tests in tools/test_execution.cpp will cover:
// - track_order() and WS subscription
// - on_order_update() state machine
// - poll_order_status() REST fallback
// - ExecutionReport generation
// - Completion callback invocation

// Placeholder for future mock-based unit tests:
// TEST(OrderTracker, TrackOrderSubscribesToWs)
// TEST(OrderTracker, OnOrderUpdateOpenToPartiallyFilled)
// TEST(OrderTracker, OnOrderUpdatePartiallyFilledToFilled)
// TEST(OrderTracker, GenerateReportCalculatesSlippage)
// TEST(OrderTracker, CompletionCallbackInvoked)

// ---------------------------------------------------------------------------
// active_orders() + recent_reports() — interface gap bridges for dashboard
//
// These tests use real WS/REST client objects constructed with a default
// (empty) ExchangeConfig. The clients are never started, so no network
// connections are made. This is sufficient to test the snapshot APIs that
// only read from internal maps populated by track_order() / stop_tracking().
// ---------------------------------------------------------------------------

class OrderTrackerSnapshotTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Construct WS/REST clients with empty config (never started).
        ExchangeConfig config;
        ws_client_ = std::make_unique<GateWsClient>(config);
        rest_client_ = std::make_unique<GateRestClient>(config);
        tracker_ = std::make_unique<OrderTracker>(*ws_client_, *rest_client_);
    }

    std::unique_ptr<GateWsClient> ws_client_;
    std::unique_ptr<GateRestClient> rest_client_;
    std::unique_ptr<OrderTracker> tracker_;
};

TEST_F(OrderTrackerSnapshotTest, ActiveOrdersEmptyOnFreshTracker)
{
    // A fresh tracker must return an empty vector from active_orders().
    const auto orders = tracker_->active_orders();
    EXPECT_TRUE(orders.empty());
}

TEST_F(OrderTrackerSnapshotTest, TrackedOrdersAppearInActiveOrders)
{
    // After track_order(), the order must appear in active_orders().
    tracker_->track_order("order_1", "BTC_USDT", Side::Buy, OrderType::Limit, 0.001, 50000.0);
    tracker_->track_order("order_2", "ETH_USDT", Side::Sell, OrderType::Market, 1.0, 3000.0);

    const auto orders = tracker_->active_orders();
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
    // After stop_tracking(), the order must no longer appear in active_orders().
    // This simulates what happens when an order reaches terminal state.
    tracker_->track_order("order_1", "BTC_USDT", Side::Buy, OrderType::Limit, 0.001, 50000.0);
    tracker_->track_order("order_2", "ETH_USDT", Side::Sell, OrderType::Market, 1.0, 3000.0);

    // Verify both are present.
    EXPECT_EQ(tracker_->active_orders().size(), 2u);

    // Stop tracking one order (simulates terminal state removal).
    tracker_->stop_tracking("order_1");

    const auto orders = tracker_->active_orders();
    ASSERT_EQ(orders.size(), 1u);
    EXPECT_EQ(orders[0].order_id, "order_2");
}

TEST_F(OrderTrackerSnapshotTest, RecentReportsEmptyOnFreshTracker)
{
    // A fresh tracker must return an empty vector from recent_reports().
    const auto reports = tracker_->recent_reports();
    EXPECT_TRUE(reports.empty());
}

TEST_F(OrderTrackerSnapshotTest, RecentReportsRespectsLimit)
{
    // recent_reports(n) must return at most n reports.
    // With no completed reports, even a large limit returns empty.
    const auto reports = tracker_->recent_reports(100);
    EXPECT_TRUE(reports.empty());
}
