// test_endpoint_router.cpp — Unit tests for EndpointRouter (Layer 1 Exchange)
//
// Tests the pure-function routing from MarketType to REST paths,
// WebSocket channel prefixes, and ping/pong channel names.
// No network connections required — pure logic tests.

#include "exchange/EndpointRouter.hpp"

#include <gtest/gtest.h>

#include <string>

namespace pulse::exchange::test
{

// ---------------------------------------------------------------------------
// Test 1: RestPrefix_Spot — spot market returns "/api/v4/spot"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, RestPrefix_Spot)
{
    EXPECT_EQ("/api/v4/spot", EndpointRouter::restPrefix(MarketType::Spot));
}

// ---------------------------------------------------------------------------
// Test 2: RestPrefix_Futures — futures market returns "/api/v4/futures/usdt"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, RestPrefix_Futures)
{
    EXPECT_EQ("/api/v4/futures/usdt", EndpointRouter::restPrefix(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 3: WsPrefix_Spot — spot WS prefix is "spot"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsPrefix_Spot)
{
    EXPECT_EQ("spot", EndpointRouter::wsPrefix(MarketType::Spot));
}

// ---------------------------------------------------------------------------
// Test 4: WsPrefix_Futures — futures WS prefix is "futures"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsPrefix_Futures)
{
    EXPECT_EQ("futures", EndpointRouter::wsPrefix(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 5: WsChannel_SpotTickers — builds "spot.tickers" from spot + "tickers"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsChannel_SpotTickers)
{
    EXPECT_EQ("spot.tickers", EndpointRouter::wsChannel(MarketType::Spot, "tickers"));
}

// ---------------------------------------------------------------------------
// Test 6: WsChannel_FuturesTickers — builds "futures.tickers" from futures + "tickers"
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsChannel_FuturesTickers)
{
    EXPECT_EQ("futures.tickers", EndpointRouter::wsChannel(MarketType::Futures, "tickers"));
}

// ---------------------------------------------------------------------------
// Test 7: PingPongChannels — spot and futures have correct ping/pong channel names
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, PingPongChannels)
{
    // Spot
    EXPECT_EQ("spot.ping", EndpointRouter::pingChannel(MarketType::Spot));
    EXPECT_EQ("spot.pong", EndpointRouter::pongChannel(MarketType::Spot));

    // Futures
    EXPECT_EQ("futures.ping", EndpointRouter::pingChannel(MarketType::Futures));
    EXPECT_EQ("futures.pong", EndpointRouter::pongChannel(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 8: SelectWsUrl — spot selects wsUrl, futures selects futuresWsUrl
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, SelectWsUrl)
{
    ExchangeConfig cfg;
    cfg.wsUrl = "wss://api.gateio.ws/ws/v4/";
    cfg.futuresWsUrl = "wss://fx-ws.gateio.ws/v4/ws/usdt";

    EXPECT_EQ("wss://api.gateio.ws/ws/v4/", EndpointRouter::selectWsUrl(cfg, MarketType::Spot));
    EXPECT_EQ("wss://fx-ws.gateio.ws/v4/ws/usdt", EndpointRouter::selectWsUrl(cfg, MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 9: NeedsJsonPing — spot requires JSON ping/pong, futures does not
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, NeedsJsonPing)
{
    EXPECT_TRUE(EndpointRouter::needsJsonPing(MarketType::Spot));
    EXPECT_FALSE(EndpointRouter::needsJsonPing(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 10: RestEndpointPaths — contracts/tickers/accounts paths are correct for both markets
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, RestEndpointPaths)
{
    // Spot paths
    EXPECT_EQ("/api/v4/spot/currency_pairs", EndpointRouter::contractsPath(MarketType::Spot));
    EXPECT_EQ("/api/v4/spot/tickers", EndpointRouter::tickersPath(MarketType::Spot));
    EXPECT_EQ("/api/v4/spot/accounts", EndpointRouter::accountsPath(MarketType::Spot));

    // Futures paths
    EXPECT_EQ("/api/v4/futures/usdt/contracts", EndpointRouter::contractsPath(MarketType::Futures));
    EXPECT_EQ("/api/v4/futures/usdt/tickers", EndpointRouter::tickersPath(MarketType::Futures));
    EXPECT_EQ("/api/v4/futures/usdt/accounts", EndpointRouter::accountsPath(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 11: WsChannel_OrderBook — builds correct channel for order book data
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsChannel_OrderBook)
{
    EXPECT_EQ("spot.order_book", EndpointRouter::wsChannel(MarketType::Spot, "order_book"));
    EXPECT_EQ("futures.order_book", EndpointRouter::wsChannel(MarketType::Futures, "order_book"));
}

// ---------------------------------------------------------------------------
// Test 12: WsChannel_PrivateChannels — builds correct channel for private data
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, WsChannel_PrivateChannels)
{
    EXPECT_EQ("spot.orders", EndpointRouter::wsChannel(MarketType::Spot, "orders"));
    EXPECT_EQ("futures.orders", EndpointRouter::wsChannel(MarketType::Futures, "orders"));
    EXPECT_EQ("spot.usertrades", EndpointRouter::wsChannel(MarketType::Spot, "usertrades"));
    EXPECT_EQ("futures.usertrades", EndpointRouter::wsChannel(MarketType::Futures, "usertrades"));
}

// ---------------------------------------------------------------------------
// Test 13: SelectWsUrl_DefaultConfig — default ExchangeConfig has correct URLs
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, SelectWsUrl_DefaultConfig)
{
    const ExchangeConfig cfg; // all defaults

    EXPECT_EQ("wss://api.gateio.ws/ws/v4/", EndpointRouter::selectWsUrl(cfg, MarketType::Spot));
    EXPECT_EQ("wss://fx-ws.gateio.ws/v4/ws/usdt", EndpointRouter::selectWsUrl(cfg, MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 14: OrdersPath_Spot — spot orders endpoint
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, OrdersPath_Spot)
{
    EXPECT_EQ("/api/v4/spot/orders", EndpointRouter::ordersPath(MarketType::Spot));
}

// ---------------------------------------------------------------------------
// Test 15: OrdersPath_Futures — futures orders endpoint
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, OrdersPath_Futures)
{
    EXPECT_EQ("/api/v4/futures/usdt/orders", EndpointRouter::ordersPath(MarketType::Futures));
}

// ---------------------------------------------------------------------------
// Test 16: OrderPath_WithId — specific order path with ID
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, OrderPath_SpotWithId)
{
    EXPECT_EQ("/api/v4/spot/orders/12345",
              EndpointRouter::orderPath(MarketType::Spot, "12345"));
}

TEST(EndpointRouterTest, OrderPath_FuturesWithId)
{
    EXPECT_EQ("/api/v4/futures/usdt/orders/67890",
              EndpointRouter::orderPath(MarketType::Futures, "67890"));
}

// ---------------------------------------------------------------------------
// Test 17: LeveragePath_Futures — futures leverage endpoint
// ---------------------------------------------------------------------------
TEST(EndpointRouterTest, LeveragePath_Futures)
{
    EXPECT_EQ("/api/v4/futures/usdt/positions/BTC_USDT/leverage",
              EndpointRouter::leveragePath(MarketType::Futures, "BTC_USDT"));
}

TEST(EndpointRouterTest, LeveragePath_Spot_Empty)
{
    EXPECT_EQ("", EndpointRouter::leveragePath(MarketType::Spot, "BTC_USDT"));
}

} // namespace pulse::exchange::test
