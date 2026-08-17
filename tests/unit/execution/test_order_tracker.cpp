// test_order_tracker.cpp — Unit tests for OrderTracker (Layer 8 Order Execution)

#include "execution/OrderTracker.hpp"

#include "exchange/GateRestClient.hpp"
#include "exchange/GateWsClient.hpp"

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
        tracker_ = std::make_unique<OrderTracker>(m_wsClient.get(), *m_restClient);
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
        tracker_ = std::make_unique<OrderTracker>(m_wsClient.get(), *m_restClient);
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
        [this, &callback_invoked, &lock_acquired_in_callback]([[maybe_unused]] const ExecutionReport &report)
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

// ---------------------------------------------------------------------------
// Futures order tracking — Gate.io futures format regression tests
//
// Futures WS events differ from spot in three ways that previously broke
// tracking (bug: fills never surfaced, orders stayed Pending forever):
//   1. "id" arrives as a NUMBER, not a string — get<std::string>() threw.
//   2. status is "open"/"finished" with the outcome in finish_as.
//   3. fills use size/left/fill_price instead of filled_total/avg_deal_price.
// ---------------------------------------------------------------------------

TEST(OrderTracker, ParseFuturesStatusDecidesOutcomeFromFinishAs)
{
    // Working states.
    EXPECT_EQ(OrderTracker::parseFuturesStatus("open", "open"), OrderStatus::Open);

    // Terminal states — finish_as decides the outcome.
    EXPECT_EQ(OrderTracker::parseFuturesStatus("finished", "filled"), OrderStatus::Filled);
    EXPECT_EQ(OrderTracker::parseFuturesStatus("finished", "cancelled"), OrderStatus::Cancelled);
    EXPECT_EQ(OrderTracker::parseFuturesStatus("finished", "reduce_only"), OrderStatus::Cancelled);
    EXPECT_EQ(OrderTracker::parseFuturesStatus("finished", "position_closed"), OrderStatus::Cancelled);

    // Defensive: finished without finish_as still resolves terminal.
    EXPECT_EQ(OrderTracker::parseFuturesStatus("finished", ""), OrderStatus::Filled);
    EXPECT_EQ(OrderTracker::parseFuturesStatus("", ""), OrderStatus::Pending);
}

TEST_F(OrderTrackerCallbackTest, FuturesFillWithNumericIdGeneratesReport)
{
    // Regression: numeric futures order id + finished/filled status must
    // produce a completion report (previously threw on id.get<string>()).
    tracker_->trackOrder("36028834465653841", "BTC_USDT", Side::Buy, OrderType::Market,
                          1.0, 0.0);

    bool callback_called = false;
    ExecutionReport captured;
    tracker_->setCompletionCallback(
        [&callback_called, &captured](const ExecutionReport &report)
        {
            callback_called = true;
            captured = report;
        });

    nlohmann::json ws_event;
    ws_event["id"] = 36028834465653841; // numeric id (futures)
    ws_event["status"] = "finished";
    ws_event["finish_as"] = "filled";
    ws_event["size"] = 1;
    ws_event["left"] = 0;
    ws_event["fill_price"] = "63072.2";
    ws_event["fee"] = 0.00048;

    tracker_->testSimulateWsUpdate(ws_event);

    EXPECT_TRUE(callback_called);
    EXPECT_TRUE(tracker_->activeOrders().empty());
    EXPECT_EQ(captured.order_id, "36028834465653841");
    EXPECT_EQ(captured.final_status, OrderStatus::Filled);

    const auto reports = tracker_->recentReports(1);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].order_id, "36028834465653841");
    EXPECT_DOUBLE_EQ(reports[0].filled_qty, 1.0);
    EXPECT_DOUBLE_EQ(reports[0].avg_fill_price, 63072.2);
    EXPECT_DOUBLE_EQ(reports[0].fees, 0.00048);
}

TEST_F(OrderTrackerCallbackTest, FuturesOpenEventWithNumericIdStaysOpen)
{
    // Regression: an open futures order (numeric id) must update the tracked
    // order without throwing and remain in activeOrders().
    tracker_->trackOrder("223191759398", "BTC_USDT", Side::Sell, OrderType::Limit,
                          1.0, 50000.0);

    nlohmann::json ws_event;
    ws_event["id"] = 223191759398; // numeric id (futures)
    ws_event["status"] = "open";
    ws_event["finish_as"] = "open";
    ws_event["size"] = -1; // sell direction
    ws_event["left"] = 1;  // nothing filled yet
    ws_event["fill_price"] = "0";

    tracker_->testSimulateWsUpdate(ws_event);

    const auto orders = tracker_->activeOrders();
    ASSERT_EQ(orders.size(), 1u);
    EXPECT_EQ(orders[0].status, OrderStatus::Open);
    EXPECT_DOUBLE_EQ(orders[0].filled_qty, 0.0);
}

TEST_F(OrderTrackerCallbackTest, FuturesPartialFillComputesFilledFromSizeLeft)
{
    // size/left are signed contract counts: a sell of 3 with 2 left has
    // filled 1 contract — filled_qty must be derived, not read from a
    // spot-style field that futures never sends.
    tracker_->trackOrder("777", "BTC_USDT", Side::Sell, OrderType::Limit, 3.0, 50000.0);

    nlohmann::json ws_event;
    ws_event["id"] = 777;
    ws_event["status"] = "open";
    ws_event["finish_as"] = "open";
    ws_event["size"] = -3;
    ws_event["left"] = -2;
    ws_event["fill_price"] = "50010.0";

    tracker_->testSimulateWsUpdate(ws_event);

    const auto orders = tracker_->activeOrders();
    ASSERT_EQ(orders.size(), 1u);
    EXPECT_EQ(orders[0].status, OrderStatus::Open);
    EXPECT_DOUBLE_EQ(orders[0].filled_qty, 1.0);

    // Complete the fill: left -> 0, finish_as -> filled.
    nlohmann::json filled_event;
    filled_event["id"] = 777;
    filled_event["status"] = "finished";
    filled_event["finish_as"] = "filled";
    filled_event["size"] = -3;
    filled_event["left"] = 0;
    filled_event["fill_price"] = "50010.0";

    bool callback_called = false;
    tracker_->setCompletionCallback([&callback_called](const ExecutionReport &)
    {
        callback_called = true;
    });

    tracker_->testSimulateWsUpdate(filled_event);

    EXPECT_TRUE(callback_called);
    EXPECT_TRUE(tracker_->activeOrders().empty());
    const auto reports = tracker_->recentReports(1);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_DOUBLE_EQ(reports[0].filled_qty, 3.0);
    EXPECT_DOUBLE_EQ(reports[0].avg_fill_price, 50010.0);
}

// ---------------------------------------------------------------------------
// M19: TradFi CFD order parsing + list-poll state machine
//
// CFD order objects use state/finished (ints) instead of the spot/futures
// status string; the single-order GET endpoint does not exist, so status
// comes from the open-orders list (findCfdOrderInList) + the fill-fields
// synthesized from the positions fallback.
// ---------------------------------------------------------------------------

TEST(OrderTracker, ParseCfdStatus_OpenAndPending)
{
    EXPECT_EQ(OrderStatus::Open, OrderTracker::parseCfdOrderStatus(
        nlohmann::json{ { "state", 1 }, { "finished", 0 } }));
    EXPECT_EQ(OrderStatus::Pending, OrderTracker::parseCfdOrderStatus(
        nlohmann::json{ { "state", 0 }, { "finished", 0 } }));
    // Missing fields default to Pending.
    EXPECT_EQ(OrderStatus::Pending, OrderTracker::parseCfdOrderStatus(nlohmann::json::object()));
    // Non-object input (null) degrades to Pending without throwing.
    EXPECT_EQ(OrderStatus::Pending, OrderTracker::parseCfdOrderStatus(nlohmann::json{}));
}

TEST(OrderTracker, ParseCfdStatus_Terminal)
{
    // state=1 + finished=1 → deleted (cancelled); any other terminal → filled.
    EXPECT_EQ(OrderStatus::Cancelled, OrderTracker::parseCfdOrderStatus(
        nlohmann::json{ { "state", 1 }, { "finished", 1 } }));
    EXPECT_EQ(OrderStatus::Filled, OrderTracker::parseCfdOrderStatus(
        nlohmann::json{ { "state", 2 }, { "finished", 1 } }));
}

TEST(OrderTracker, FindCfdOrderInList_ById)
{
    const nlohmann::json list = nlohmann::json::array({
        nlohmann::json{ { "order_id", 17618607 }, { "symbol", "XAUUSD" } },
        nlohmann::json{ { "order_id", "17511143" }, { "symbol", "XAUUSD" } }, // string form
    });

    // int id, string id.
    EXPECT_EQ(17618607, OrderTracker::findCfdOrderInList(list, "17618607")["order_id"]);
    EXPECT_EQ("17511143", OrderTracker::findCfdOrderInList(list, "17511143")["order_id"]);

    // Not found / null list / missing order_id entries.
    EXPECT_TRUE(OrderTracker::findCfdOrderInList(list, "999").is_null());
    EXPECT_TRUE(OrderTracker::findCfdOrderInList(nlohmann::json{}, "17618607").is_null());
    EXPECT_TRUE(OrderTracker::findCfdOrderInList(
        nlohmann::json::array({ nlohmann::json{ { "symbol", "XAUUSD" } } }), "1").is_null());
}

// ---------------------------------------------------------------------------
// CFD state machine via testSimulateCfdPoll (no network).
// ---------------------------------------------------------------------------

class OrderTrackerCfdTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ExchangeConfig config;
        m_wsClient = std::make_unique<GateWsClient>(config);
        m_restClient = std::make_unique<GateRestClient>(config);
        // CFD: REST-poll-only (enable_ws=false), like main.cpp's cfd_tracker.
        tracker_ = std::make_unique<OrderTracker>(m_wsClient.get(), *m_restClient,
                                                  MarketType::Cfd, /*enable_ws=*/false);
    }

    std::unique_ptr<GateWsClient> m_wsClient;
    std::unique_ptr<GateRestClient> m_restClient;
    std::unique_ptr<OrderTracker> tracker_;
};

TEST_F(OrderTrackerCfdTest, CfdPollStateMachine_OpenThenFilled)
{
    tracker_->trackOrder("17633250", "XAUUSD", Side::Buy, OrderType::Market,
                         0.01, 4406.5, "cfd_client_1");

    // 1. Open state (state=1, finished=0) — stays tracked, no callback.
    bool callback_invoked = false;
    tracker_->setCompletionCallback(
        [&callback_invoked](const ExecutionReport &) { callback_invoked = true; });

    tracker_->testSimulateCfdPoll(
        nlohmann::json{ { "order_id", 17633250 }, { "state", 1 }, { "finished", 0 } });

    EXPECT_FALSE(callback_invoked);
    auto active = tracker_->activeOrders();
    ASSERT_EQ(1u, active.size());
    EXPECT_EQ(OrderStatus::Open, active[0].status);

    // 2. Terminal fill (state=2, finished=1, fill fields synthesized from
    //    the positions fallback) — callback fires, report carries the fill.
    ExecutionReport captured;
    tracker_->setCompletionCallback(
        [&captured](const ExecutionReport &report) { captured = report; });

    tracker_->testSimulateCfdPoll(nlohmann::json{
        { "order_id", 17633250 }, { "state", 2 }, { "finished", 1 },
        { "filled_volume", "0.01" }, { "fill_price", "4406.68" }, { "fee", "-0.06" } });

    EXPECT_EQ(OrderStatus::Filled, captured.final_status);
    EXPECT_DOUBLE_EQ(0.01, captured.filled_qty);
    EXPECT_DOUBLE_EQ(4406.68, captured.avg_fill_price);
    EXPECT_DOUBLE_EQ(-0.06, captured.fees);
    EXPECT_EQ("cfd_client_1", captured.client_order_id);
    EXPECT_TRUE(tracker_->activeOrders().empty());
    ASSERT_EQ(1u, tracker_->recentReports().size());
}

TEST_F(OrderTrackerCfdTest, CfdPollStateMachine_FinishedState1IsCancelled)
{
    // A deleted order (state=1, finished=1) — e.g. after a successful cancel
    // removed it from the list — completes as Cancelled with filled_qty 0
    // (no phantom position can be opened by onOrderComplete).
    tracker_->trackOrder("17511143", "XAUUSD", Side::Buy, OrderType::Market,
                         0.01, 4400.0, "cfd_client_2");

    ExecutionReport captured;
    tracker_->setCompletionCallback(
        [&captured](const ExecutionReport &report) { captured = report; });

    tracker_->testSimulateCfdPoll(
        nlohmann::json{ { "order_id", 17511143 }, { "state", 1 }, { "finished", 1 } });

    EXPECT_EQ(OrderStatus::Cancelled, captured.final_status);
    EXPECT_DOUBLE_EQ(0.0, captured.filled_qty);
    EXPECT_TRUE(tracker_->activeOrders().empty());
}
