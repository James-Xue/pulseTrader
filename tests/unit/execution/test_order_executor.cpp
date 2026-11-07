// test_order_executor.cpp — Unit tests for OrderExecutor (Layer 8 Order Execution)

#include "execution/OrderExecutor.hpp"

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

// buildOrderBody() is a public static since M15 — the order body formats are
// unit-tested here without any network.

TEST(OrderExecutor, BuildOrderBodyCfdMarketBuy)
{
    OrderRequest req;
    req.symbol = "XAUUSD";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 0.01;

    const auto body = OrderExecutor::buildOrderBody(MarketType::Cfd, req);
    EXPECT_EQ("XAUUSD", body["symbol"].get<std::string>());
    EXPECT_EQ(2, body["side"].get<int>());       // 2 = buy
    EXPECT_EQ("0.010000", body["volume"].get<std::string>());
    EXPECT_EQ("market", body["price_type"].get<std::string>());
    EXPECT_FALSE(body.contains("price"));
    EXPECT_FALSE(body.contains("contract"));
    EXPECT_FALSE(body.contains("size"));
}

TEST(OrderExecutor, BuildOrderBodyCfdLimitSell)
{
    OrderRequest req;
    req.symbol = "XAUUSD";
    req.side = Side::Sell;
    req.type = OrderType::Limit;
    req.quantity = 0.01;
    req.price = 4400.0;

    const auto body = OrderExecutor::buildOrderBody(MarketType::Cfd, req);
    EXPECT_EQ(1, body["side"].get<int>());       // 1 = sell
    EXPECT_EQ("trigger", body["price_type"].get<std::string>());
    EXPECT_EQ("4400.000000", body["price"].get<std::string>());
}

TEST(OrderExecutor, BuildOrderBodyCfdOmitsClientText)
{
    // The MT5-style CFD schema has no "text" field — client ids are skipped.
    OrderRequest req;
    req.symbol = "XAUUSD";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 0.01;
    req.client_order_id = "abc";

    const auto body = OrderExecutor::buildOrderBody(MarketType::Cfd, req);
    EXPECT_FALSE(body.contains("text"));
}

TEST(OrderExecutor, BuildOrderBodyFuturesRegression)
{
    // Futures body must be unchanged after the static refactor (M15).
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.contract_size = 1;
    req.reduce_only = true;

    const auto body = OrderExecutor::buildOrderBody(MarketType::Futures, req);
    EXPECT_EQ("BTC_USDT", body["contract"].get<std::string>());
    EXPECT_EQ(-1, body["size"].get<int>());
    EXPECT_EQ("0", body["price"].get<std::string>());
    EXPECT_EQ("ioc", body["tif"].get<std::string>());
    EXPECT_TRUE(body["reduce_only"].get<bool>());
}

// ---------------------------------------------------------------------------
// M17: CFD order-id resolution (POST /tradfi/orders does not echo the id)
// ---------------------------------------------------------------------------

namespace
{

// Anchor for the match window — the fixture's fresh entries are placed at
// this instant; the stale leftover (17511143 = the 08-14 buy@4295) is one
// day old and must never be matched by a new order with the same key.
constexpr std::int64_t kPlacedAt = 1786000000;

// Simulated data.list of GET /tradfi/orders (newest-first, real field shapes:
// order_id as int, volume/price as strings, side 2=buy / 1=sell).
nlohmann::json cfd_orders_list()
{
    return nlohmann::json::array({
        nlohmann::json{ { "order_id", 17633250 }, { "symbol", "XAUUSD" },
                        { "side", 2 },              { "volume", "0.01" },
                        { "price", "1000.00" },     { "price_type", "trigger" },
                        { "time_setup", kPlacedAt } },
        nlohmann::json{ { "order_id", 17618607 }, { "symbol", "XAUUSD" },
                        { "side", 1 },              { "volume", "0.01" },
                        { "price", "4418.00" },     { "price_type", "trigger" },
                        { "time_setup", kPlacedAt } },
        nlohmann::json{ { "order_id", 17511143 }, { "symbol", "XAUUSD" },
                        { "side", 2 },              { "volume", "0.01" },
                        { "price", "4295.00" },     { "price_type", "trigger" },
                        { "time_setup", kPlacedAt - 86400 } },
    });
}

OrderRequest cfd_request(Side side, double qty, double price)
{
    OrderRequest req;
    req.symbol = "XAUUSD";
    req.side = side;
    req.type = OrderType::Limit;
    req.quantity = qty;
    req.price = price;
    return req;
}

} // namespace

TEST(OrderExecutor, MatchCfdOrderId_FindsNewestMatchingTrigger)
{
    // Newest-first list; the placed trigger buy @1000 matches the first entry.
    const auto id = OrderExecutor::matchCfdOrderId(
        cfd_orders_list(), cfd_request(Side::Buy, 0.01, 1000.0), kPlacedAt);
    EXPECT_EQ("17633250", id);
}

TEST(OrderExecutor, MatchCfdOrderId_IgnoresWrongSideAndPrice)
{
    // Sell @1000 exists nowhere in the list: the sell entries are @4418/@4295
    // (buy) — no match despite the same symbol/side/volume.
    const auto id = OrderExecutor::matchCfdOrderId(
        cfd_orders_list(), cfd_request(Side::Sell, 0.01, 1000.0), kPlacedAt);
    EXPECT_TRUE(id.empty());
}

TEST(OrderExecutor, MatchCfdOrderId_ReturnsEmptyWhenNothingMatches)
{
    const auto id = OrderExecutor::matchCfdOrderId(
        cfd_orders_list(), cfd_request(Side::Sell, 0.05, 1000.0), kPlacedAt);
    EXPECT_TRUE(id.empty());
}

TEST(OrderExecutor, MatchCfdOrderId_MarketOrderMatchesWithoutPrice)
{
    OrderRequest req = cfd_request(Side::Sell, 0.01, 0.0);
    req.type = OrderType::Market;

    // The sell @4418 entry matches on symbol/side/volume alone.
    const auto id = OrderExecutor::matchCfdOrderId(cfd_orders_list(), req, kPlacedAt);
    EXPECT_EQ("17618607", id);
}

TEST(OrderExecutor, MatchCfdOrderId_HandlesStringOrderId)
{
    auto list = cfd_orders_list();
    list[0]["order_id"] = "17633250"; // string form
    const auto id = OrderExecutor::matchCfdOrderId(
        list, cfd_request(Side::Buy, 0.01, 1000.0), kPlacedAt);
    EXPECT_EQ("17633250", id);
}

// ---------------------------------------------------------------------------
// M19: match window — the 2026-08-17 incident regression
// (a new market buy 0.01 matched the stale buy@4295 and the later cancel
// deleted the user's leftover trigger).
// ---------------------------------------------------------------------------

TEST(OrderExecutor, MatchCfdOrderId_MarketTimeWindowExcludesLegacySameKey)
{
    // A NEW market buy 0.01 has the exact same symbol/side/volume as the
    // stale leftover 17511143 — the legacy entry is one day old and must be
    // skipped; only the fresh entry (newest-first) can be ours.
    OrderRequest req = cfd_request(Side::Buy, 0.01, 0.0);
    req.type = OrderType::Market;

    const auto id = OrderExecutor::matchCfdOrderId(cfd_orders_list(), req, kPlacedAt);
    EXPECT_EQ("17633250", id); // The fresh buy @1000 — never 17511143.
}

TEST(OrderExecutor, MatchCfdOrderId_TimeWindowBoundary)
{
    OrderRequest req = cfd_request(Side::Buy, 0.01, 0.0);
    req.type = OrderType::Market;

    auto list = cfd_orders_list();

    // time_setup == placed_at - 5s — inside the window (slack inclusive).
    list[0]["time_setup"] = kPlacedAt - 5;
    EXPECT_EQ("17633250", OrderExecutor::matchCfdOrderId(list, req, kPlacedAt));

    // time_setup == placed_at - 6s — outside the window: no fresh buy match.
    list[0]["time_setup"] = kPlacedAt - 6;
    EXPECT_TRUE(OrderExecutor::matchCfdOrderId(list, req, kPlacedAt).empty());
}

TEST(OrderExecutor, MatchCfdOrderId_NewestWithinWindowWins)
{
    // Two same-key buys within the window — the newest-first entry wins.
    auto list = cfd_orders_list();
    list[0]["time_setup"] = kPlacedAt;          // newest
    list[1] = nlohmann::json{ { "order_id", 99999999 }, { "symbol", "XAUUSD" },
                              { "side", 2 }, { "volume", "0.01" },
                              { "price_type", "market" },
                              { "time_setup", kPlacedAt - 3 } };
    list.erase(list.begin() + 2); // drop the stale entry

    OrderRequest req = cfd_request(Side::Buy, 0.01, 0.0);
    req.type = OrderType::Market;
    EXPECT_EQ("17633250", OrderExecutor::matchCfdOrderId(list, req, kPlacedAt));
}

// ---------------------------------------------------------------------------
// M19: 2xx business-error detection ({"label","message"} bodies)
// ---------------------------------------------------------------------------

TEST(OrderExecutor, CfdBusinessError_DetectsLabelBody)
{
    const nlohmann::json err = nlohmann::json::parse(R"({
        "label": "NOT_IN_TRADE",
        "message": "Market currently closed",
        "data": null
    })");

    const auto result = OrderExecutor::cfdBusinessError(err);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(ErrorCode::OrderRejected, result->code);
    EXPECT_NE(std::string::npos, result->message.find("NOT_IN_TRADE"));
}

TEST(OrderExecutor, CfdBusinessError_OkOnDataBody)
{
    const nlohmann::json ok = nlohmann::json::parse(R"({
        "data": { "id": "43713", "log_id": "15853632" }
    })");

    EXPECT_FALSE(OrderExecutor::cfdBusinessError(ok).has_value());
}

// ---------------------------------------------------------------------------
// M12: Futures-specific OrderRequest tests
// ---------------------------------------------------------------------------

TEST(OrderRequest, FuturesFieldsDefaultSpot)
{
    OrderRequest req;
    EXPECT_EQ(MarketType::Spot, req.market_type);
    EXPECT_DOUBLE_EQ(0.0, req.leverage); // 0 = do not manage leverage
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
    EXPECT_DOUBLE_EQ(0.0, req.leverage); // 0 = do not manage leverage
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
