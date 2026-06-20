// test_endpoint_router.cpp — Unit tests for EndpointRouter (Layer 1 Exchange)
//
// Tests the pure-function routing from MarketType to REST paths,
// WebSocket channel prefixes, and ping/pong channel names.
// No network connections required — pure logic tests.

#include "exchange/endpoint_router.hpp"

#include <gtest/gtest.h>

#include <string>

namespace pulse::exchange::test
{

// ---------------------------------------------------------------------------
// Test 1: RestPrefix_Spot — spot market returns "/api/v4/spot"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, RestPrefix_Spot)
{
    EXPECT_EQ("/api/v4/spot", EndpointRouter::rest_prefix(MarketType::Spot));
}

// ---------------------------------------------------------------------------
// Test 2: RestPrefix_Futures — futures market returns "/api/v4/futures/usdt"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, RestPrefix_Futures)
{
    EXPECT_EQ("/api/v4/futures/usdt", EndpointRouter::rest_prefix(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 3: WsPrefix_Spot — spot WS prefix is "spot"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsPrefix_Spot)
{
    EXPECT_EQ("spot", EndpointRouter::ws_prefix(MarketType::Spot));
}

// ---------------------------------------------------------------------------
// Test 4: WsPrefix_Futures — futures WS prefix is "futures"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsPrefix_Futures)
{
    EXPECT_EQ("futures", EndpointRouter::ws_prefix(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 5: WsChannel_SpotTickers — builds "spot.tickers" from spot + "tickers"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsChannel_SpotTickers)
{
    EXPECT_EQ("spot.tickers", EndpointRouter::ws_channel(MarketType::Spot, "tickers"));
}

// ---------------------------------------------------------------------------
// Test 6: WsChannel_FuturesTickers — builds "futures.tickers" from futures + "tickers"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsChannel_FuturesTickers)
{
    EXPECT_EQ("futures.tickers", EndpointRouter::ws_channel(MarketType::Futures, "tickers"));
}

// ---------------------------------------------------------------------------
// Test 7: PingPongChannels — spot and futures have correct ping/pong channel names
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, PingPongChannels)
{
    // Spot
    EXPECT_EQ("spot.ping", EndpointRouter::ping_channel(MarketType::Spot));
    EXPECT_EQ("spot.pong", EndpointRouter::pong_channel(MarketType::Spot));

    // Futures
    EXPECT_EQ("futures.ping", EndpointRouter::ping_channel(MarketType::Futures));
    EXPECT_EQ("futures.pong", EndpointRouter::pong_channel(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 8: SelectWsUrl — spot selects wsUrl, futures selects futuresWsUrl
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, SelectWsUrl)
{
    ExchangeConfig cfg;
    cfg.wsUrl = "wss://api.gateio.ws/ws/v4/";
    cfg.futuresWsUrl = "wss://fx-ws.gateio.ws/v4/ws/usdt";

    EXPECT_EQ("wss://api.gateio.ws/ws/v4/", EndpointRouter::select_ws_url(cfg, MarketType::Spot));
    EXPECT_EQ("wss://fx-ws.gateio.ws/v4/ws/usdt", EndpointRouter::select_ws_url(cfg, MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 9: NeedsJsonPing — spot requires JSON ping/pong, futures does not
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, NeedsJsonPing)
{
    EXPECT_TRUE(EndpointRouter::needs_json_ping(MarketType::Spot));
    EXPECT_FALSE(EndpointRouter::needs_json_ping(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 10: RestEndpointPaths — contracts/tickers/accounts paths are correct for both markets
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, RestEndpointPaths)
{
    // Spot paths
    EXPECT_EQ("/api/v4/spot/currency_pairs", EndpointRouter::contracts_path(MarketType::Spot));
    EXPECT_EQ("/api/v4/spot/tickers", EndpointRouter::tickers_path(MarketType::Spot));
    EXPECT_EQ("/api/v4/spot/accounts", EndpointRouter::accounts_path(MarketType::Spot));

    // Futures paths
    EXPECT_EQ("/api/v4/futures/usdt/contracts", EndpointRouter::contracts_path(MarketType::Futures));
    EXPECT_EQ("/api/v4/futures/usdt/tickers", EndpointRouter::tickers_path(MarketType::Futures));
    EXPECT_EQ("/api/v4/futures/usdt/accounts", EndpointRouter::accounts_path(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 11: WsChannel_OrderBook — builds correct channel for order book data
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsChannel_OrderBook)
{
    EXPECT_EQ("spot.order_book", EndpointRouter::ws_channel(MarketType::Spot, "order_book"));
    EXPECT_EQ("futures.order_book", EndpointRouter::ws_channel(MarketType::Futures, "order_book"));
}

// ---------------------------------------------------------------------------
// Test 12: WsChannel_PrivateChannels — builds correct channel for private data
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsChannel_PrivateChannels)
{
    EXPECT_EQ("spot.orders", EndpointRouter::ws_channel(MarketType::Spot, "orders"));
    EXPECT_EQ("futures.orders", EndpointRouter::ws_channel(MarketType::Futures, "orders"));
    EXPECT_EQ("spot.usertrades", EndpointRouter::ws_channel(MarketType::Spot, "usertrades"));
    EXPECT_EQ("futures.usertrades", EndpointRouter::ws_channel(MarketType::Futures, "usertrades"));
}

// ---------------------------------------------------------------------------
// Test 13: SelectWsUrl_DefaultConfig — default ExchangeConfig has correct URLs
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, SelectWsUrl_DefaultConfig)
{
    const ExchangeConfig cfg; // all defaults

    EXPECT_EQ("wss://api.gateio.ws/ws/v4/", EndpointRouter::select_ws_url(cfg, MarketType::Spot));
    EXPECT_EQ("wss://fx-ws.gateio.ws/v4/ws/usdt", EndpointRouter::select_ws_url(cfg, MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 14: OrdersPath_Spot — spot orders endpoint
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, OrdersPath_Spot)
{
    EXPECT_EQ("/api/v4/spot/orders", EndpointRouter::orders_path(MarketType::Spot));
}

// ---------------------------------------------------------------------------
// Test 15: OrdersPath_Futures — futures orders endpoint
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, OrdersPath_Futures)
{
    EXPECT_EQ("/api/v4/futures/usdt/orders", EndpointRouter::orders_path(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 16: OrderPath_WithId — specific order path with ID
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, OrderPath_SpotWithId)
{
    EXPECT_EQ("/api/v4/spot/orders/12345",
              EndpointRouter::order_path(MarketType::Spot, "12345"));
}

TEST(EndpointRouterTest, OrderPath_FuturesWithId)
{
    EXPECT_EQ("/api/v4/futures/usdt/orders/67890",
              EndpointRouter::order_path(MarketType::Futures, "67890"));
}

// ---------------------------------------------------------------------------
// Test 17: LeveragePath_Futures — futures leverage endpoint
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, LeveragePath_Futures)
{
    EXPECT_EQ("/api/v4/futures/usdt/positions/BTC_USDT/leverage",
              EndpointRouter::leverage_path(MarketType::Futures, "BTC_USDT"));
}

TEST(EndpointRouterTest, LeveragePath_Spot_Empty)
{
    EXPECT_EQ("", EndpointRouter::leverage_path(MarketType::Spot, "BTC_USDT"));
}

} // namespace pulse::exchange::test
