// test_order_executor.cpp — Unit tests for OrderExecutor (Layer 8 Order Execution)

#include "pulse/execution/order_executor.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::execution;

// ---------------------------------------------------------------------------
// OrderRequest construction
// ---------------------------------------------------------------------------

TEST(OrderRequest, DefaultConstruction)
{
    OrderRequest req;
    EXPECT_EQ(req.symbol, "");
    EXPECT_EQ(req.side, Side::Buy);
    EXPECT_EQ(req.type, OrderType::Limit);
    EXPECT_DOUBLE_EQ(req.quantity, 0.0);
    EXPECT_DOUBLE_EQ(req.price, 0.0);
    EXPECT_EQ(req.client_order_id, "");
}

TEST(OrderRequest, ManualConstruction)
{
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Sell;
    req.type = OrderType::Limit;
    req.quantity = 0.001;
    req.price = 50000.0;
    req.client_order_id = "client_001";

    EXPECT_EQ(req.symbol, "BTC_USDT");
    EXPECT_EQ(req.side, Side::Sell);
    EXPECT_DOUBLE_EQ(req.quantity, 0.001);
}

// ---------------------------------------------------------------------------
// OrderResponse construction
// ---------------------------------------------------------------------------

TEST(OrderResponse, DefaultConstruction)
{
    OrderResponse resp;
    EXPECT_EQ(resp.order_id, "");
    EXPECT_EQ(resp.status, OrderStatus::Pending);
}

// ---------------------------------------------------------------------------
// OrderExecutor (requires REST client — tested via integration tests)
// ---------------------------------------------------------------------------

// Note: build_order_body() and parse_order_response() are private methods.
// Full OrderExecutor testing requires a real or mock GateRestClient.
// These tests would be integration tests in tools/test_execution.cpp.

// Placeholder for future mock-based unit tests:
// TEST(OrderExecutor, BuildOrderBodyLimitBuy)
// TEST(OrderExecutor, BuildOrderBodyMarketSell)
// TEST(OrderExecutor, BuildOrderBodyPostOnly)
// TEST(OrderExecutor, ParseOrderResponseOpen)
// TEST(OrderExecutor, ParseOrderResponseClosed)
// TEST(OrderExecutor, ParseOrderResponseCancelled)
