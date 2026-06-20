// test_order_executor.cpp — Unit tests for OrderExecutor (Layer 8 Order Execution)

#include "execution/order_executor.hpp"

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

// ---------------------------------------------------------------------------
// M12: Futures-specific OrderRequest tests
// ---------------------------------------------------------------------------

TEST(OrderRequest, FuturesFieldsDefaultSpot)
{
    OrderRequest req;
    EXPECT_EQ(MarketType::Spot, req.market_type);
    EXPECT_DOUBLE_EQ(1.0, req.leverage);
    EXPECT_FALSE(req.reduce_only);
    EXPECT_EQ(0, req.contract_size);
}

TEST(OrderRequest, FuturesFieldsSetCorrectly)
{
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.quantity = 100;
    req.price = 50000.0;
    req.market_type = MarketType::Futures;
    req.leverage = 10.0;
    req.reduce_only = true;
    req.contract_size = 50;

    EXPECT_EQ(MarketType::Futures, req.market_type);
    EXPECT_DOUBLE_EQ(10.0, req.leverage);
    EXPECT_TRUE(req.reduce_only);
    EXPECT_EQ(50, req.contract_size);
}

TEST(OrderRequest, SpotDefaultsBackwardCompatible)
{
    // Verify that spot-default OrderRequest behaves identically to pre-M12.
    OrderRequest req;
    req.symbol = "ETH_USDT";
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.price = 3000.0;

    // All futures fields at defaults — spot behavior unchanged.
    EXPECT_EQ(MarketType::Spot, req.market_type);
    EXPECT_DOUBLE_EQ(1.0, req.leverage);
    EXPECT_FALSE(req.reduce_only);
    EXPECT_EQ(0, req.contract_size);
}

// ---------------------------------------------------------------------------
// M12: TradingSignal market_type test
// ---------------------------------------------------------------------------

TEST(OrderRequest, MarketTypeRouting)
{
    // Verify that market_type can be used to route to different executors.
    OrderRequest spot_req;
    spot_req.market_type = MarketType::Spot;

    OrderRequest futures_req;
    futures_req.market_type = MarketType::Futures;

    EXPECT_NE(spot_req.market_type, futures_req.market_type);
    EXPECT_EQ(MarketType::Spot, spot_req.market_type);
    EXPECT_EQ(MarketType::Futures, futures_req.market_type);
}
