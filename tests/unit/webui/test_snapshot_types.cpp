// test_snapshot_types.cpp — Unit tests for WebUI snapshot structs (Layer 9 WebUI)
//
// Tests default construction, JSON serialization, and field correctness for
// each snapshot struct defined in snapshot_types.hpp.
//
// Tests:
//   1. DefaultOrderBookSnapshotIsEmpty — empty bids/asks vectors
//   2. OrderBookSnapshotToJson         — 2 bids + 2 asks, verify JSON keys
//   3. KlineSnapshotToJson             — 2 candles, verify JSON array
//   4. PositionsSnapshotToJson         — 1 position, verify fields
//   5. OrdersSnapshotToJson            — 1 active order, verify fields
//   6. MetricsSnapshotUnavailable      — available=false, verify JSON
//   7. AiSnapshotUnavailable           — available=false, verify JSON
//   8. DashboardSnapshotToJson         — full snapshot with all panels

#include "webui/snapshot_types.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using namespace pulse;
using namespace pulse::webui;

// ---------------------------------------------------------------------------
// 1. DefaultOrderBookSnapshotIsEmpty
// ---------------------------------------------------------------------------

TEST(SnapshotTypes, DefaultOrderBookSnapshotIsEmpty)
{
    OrderBookSnapshot snap;

    // Default-constructed snapshot must have empty bids/asks.
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
    EXPECT_EQ(0u, snap.sequence_id);
    EXPECT_EQ(0, snap.timestamp);
    EXPECT_TRUE(snap.symbol.empty());
}

// ---------------------------------------------------------------------------
// 2. OrderBookSnapshotToJson
// ---------------------------------------------------------------------------

TEST(SnapshotTypes, OrderBookSnapshotToJson)
{
    OrderBookSnapshot snap;
    snap.symbol = "BTC_USDT";
    snap.sequence_id = 42;
    snap.timestamp = 1700000000000;

    // 2 bid levels.
    snap.bids.push_back({50000.0, 1.5});
    snap.bids.push_back({49999.0, 2.0});

    // 2 ask levels.
    snap.asks.push_back({50001.0, 0.8});
    snap.asks.push_back({50002.0, 1.2});

    nlohmann::json j = snap;

    // Verify top-level keys.
    EXPECT_EQ("BTC_USDT", j["symbol"].get<std::string>());
    EXPECT_EQ(42u, j["sequence_id"].get<std::uint64_t>());
    EXPECT_EQ(1700000000000, j["timestamp"].get<std::int64_t>());

    // Verify bids array.
    ASSERT_TRUE(j["bids"].is_array());
    ASSERT_EQ(2u, j["bids"].size());
    EXPECT_DOUBLE_EQ(50000.0, j["bids"][0]["price"].get<double>());
    EXPECT_DOUBLE_EQ(1.5, j["bids"][0]["quantity"].get<double>());
    EXPECT_DOUBLE_EQ(49999.0, j["bids"][1]["price"].get<double>());

    // Verify asks array.
    ASSERT_TRUE(j["asks"].is_array());
    ASSERT_EQ(2u, j["asks"].size());
    EXPECT_DOUBLE_EQ(50001.0, j["asks"][0]["price"].get<double>());
    EXPECT_DOUBLE_EQ(0.8, j["asks"][0]["quantity"].get<double>());
}

// ---------------------------------------------------------------------------
// 3. KlineSnapshotToJson
// ---------------------------------------------------------------------------

TEST(SnapshotTypes, KlineSnapshotToJson)
{
    KlineSnapshot snap;
    snap.symbol = "ETH_USDT";

    // 2 candles.
    market::Kline k1;
    k1.open_time = 1700000000000;
    k1.close_time = 1700000060000;
    k1.open = 3000.0;
    k1.high = 3050.0;
    k1.low = 2990.0;
    k1.close = 3040.0;
    k1.volume = 123.45;
    k1.closed = true;

    market::Kline k2;
    k2.open_time = 1700000060000;
    k2.close_time = 1700000120000;
    k2.open = 3040.0;
    k2.high = 3060.0;
    k2.low = 3035.0;
    k2.close = 3055.0;
    k2.volume = 98.76;
    k2.closed = false;

    snap.candles.push_back(k1);
    snap.candles.push_back(k2);

    nlohmann::json j = snap;

    EXPECT_EQ("ETH_USDT", j["symbol"].get<std::string>());
    ASSERT_TRUE(j["candles"].is_array());
    ASSERT_EQ(2u, j["candles"].size());

    // Verify first candle fields.
    EXPECT_EQ(1700000000000, j["candles"][0]["open_time"].get<std::int64_t>());
    EXPECT_DOUBLE_EQ(3000.0, j["candles"][0]["open"].get<double>());
    EXPECT_DOUBLE_EQ(3050.0, j["candles"][0]["high"].get<double>());
    EXPECT_DOUBLE_EQ(2990.0, j["candles"][0]["low"].get<double>());
    EXPECT_DOUBLE_EQ(3040.0, j["candles"][0]["close"].get<double>());
    EXPECT_DOUBLE_EQ(123.45, j["candles"][0]["volume"].get<double>());
    EXPECT_TRUE(j["candles"][0]["closed"].get<bool>());

    // Verify second candle.
    EXPECT_FALSE(j["candles"][1]["closed"].get<bool>());
}

// ---------------------------------------------------------------------------
// 4. PositionsSnapshotToJson
// ---------------------------------------------------------------------------

TEST(SnapshotTypes, PositionsSnapshotToJson)
{
    PositionsSnapshot snap;

    // 1 position.
    risk::Position pos;
    pos.position_id = "BTC_USDT_Buy_1";
    pos.symbol = "BTC_USDT";
    pos.side = Side::Buy;
    pos.quantity = 0.01;
    pos.entry_price = 50000.0;
    pos.current_price = 50500.0;
    pos.unrealized_pnl = 5.0;
    pos.notional_value = 505.0;
    pos.open_time = Timestamp{ std::chrono::milliseconds{ 1700000000000 } };
    pos.strategy_id = "momentum_scalper_BTC_USDT";

    snap.positions.push_back(pos);

    // Portfolio summary.
    snap.portfolio.openPositionCount = 1;
    snap.portfolio.total_notional = 505.0;
    snap.portfolio.total_unrealized_pnl = 5.0;
    snap.portfolio.net_exposure = 505.0;

    nlohmann::json j = snap;

    // Verify positions array.
    ASSERT_TRUE(j["positions"].is_array());
    ASSERT_EQ(1u, j["positions"].size());

    const auto &jp = j["positions"][0];
    EXPECT_EQ("BTC_USDT_Buy_1", jp["position_id"].get<std::string>());
    EXPECT_EQ("BTC_USDT", jp["symbol"].get<std::string>());
    EXPECT_EQ("buy", jp["side"].get<std::string>());
    EXPECT_DOUBLE_EQ(0.01, jp["quantity"].get<double>());
    EXPECT_DOUBLE_EQ(50000.0, jp["entry_price"].get<double>());
    EXPECT_DOUBLE_EQ(50500.0, jp["current_price"].get<double>());
    EXPECT_DOUBLE_EQ(5.0, jp["unrealized_pnl"].get<double>());
    EXPECT_EQ("momentum_scalper_BTC_USDT", jp["strategy_id"].get<std::string>());

    // Verify portfolio summary.
    EXPECT_EQ(1, j["portfolio"]["openPositionCount"].get<int>());
    EXPECT_DOUBLE_EQ(505.0, j["portfolio"]["total_notional"].get<double>());
}

// ---------------------------------------------------------------------------
// 5. OrdersSnapshotToJson
// ---------------------------------------------------------------------------

TEST(SnapshotTypes, OrdersSnapshotToJson)
{
    OrdersSnapshot snap;

    // 1 active order.
    execution::OrderSnapshot order;
    order.order_id = "order_123";
    order.symbol = "BTC_USDT";
    order.side = Side::Sell;
    order.type = OrderType::Limit;
    order.requested_qty = 0.005;
    order.filled_qty = 0.002;
    order.status = OrderStatus::Open;
    order.submit_time = Timestamp{ std::chrono::milliseconds{ 1700000000000 } };
    order.last_update_time = Timestamp{ std::chrono::milliseconds{ 1700000001000 } };

    snap.activeOrders.push_back(order);

    nlohmann::json j = snap;

    // Verify activeOrders array.
    ASSERT_TRUE(j["activeOrders"].is_array());
    ASSERT_EQ(1u, j["activeOrders"].size());

    const auto &jo = j["activeOrders"][0];
    EXPECT_EQ("order_123", jo["order_id"].get<std::string>());
    EXPECT_EQ("BTC_USDT", jo["symbol"].get<std::string>());
    EXPECT_EQ("sell", jo["side"].get<std::string>());
    EXPECT_EQ("limit", jo["type"].get<std::string>());
    EXPECT_DOUBLE_EQ(0.005, jo["requested_qty"].get<double>());
    EXPECT_DOUBLE_EQ(0.002, jo["filled_qty"].get<double>());
    EXPECT_EQ("open", jo["status"].get<std::string>());

    // Verify recentReports is empty.
    ASSERT_TRUE(j["recentReports"].is_array());
    EXPECT_TRUE(j["recentReports"].empty());
}

// ---------------------------------------------------------------------------
// 6. MetricsSnapshotUnavailable
// ---------------------------------------------------------------------------

TEST(SnapshotTypes, MetricsSnapshotUnavailable)
{
    MetricsSnapshot snap;

    // Default-constructed metrics must be unavailable.
    EXPECT_FALSE(snap.available);
    EXPECT_DOUBLE_EQ(0.0, snap.net_pnl);
    EXPECT_EQ(0, snap.tradeCount);

    nlohmann::json j = snap;

    EXPECT_FALSE(j["available"].get<bool>());
    EXPECT_DOUBLE_EQ(0.0, j["net_pnl"].get<double>());
    EXPECT_DOUBLE_EQ(0.0, j["gross_pnl"].get<double>());
    EXPECT_DOUBLE_EQ(0.0, j["win_rate"].get<double>());
    EXPECT_DOUBLE_EQ(0.0, j["avg_win_loss_ratio"].get<double>());
    EXPECT_DOUBLE_EQ(0.0, j["sharpe_ratio"].get<double>());
    EXPECT_DOUBLE_EQ(0.0, j["maxDrawdown"].get<double>());
    EXPECT_EQ(0, j["tradeCount"].get<int>());
}

// ---------------------------------------------------------------------------
// 7. AiSnapshotUnavailable
// ---------------------------------------------------------------------------

TEST(SnapshotTypes, AiSnapshotUnavailable)
{
    AiSnapshot snap;

    // Default-constructed AI snapshot must be unavailable.
    EXPECT_FALSE(snap.available);
    EXPECT_EQ(0, snap.last_update_ms);

    nlohmann::json j = snap;

    EXPECT_FALSE(j["available"].get<bool>());
    EXPECT_EQ(0, j["last_update_ms"].get<std::int64_t>());

    // result should still serialize (with defaults).
    EXPECT_TRUE(j.contains("result"));
    EXPECT_EQ("neutral", j["result"]["sentiment"].get<std::string>());
    EXPECT_DOUBLE_EQ(0.0, j["result"]["confidence"].get<double>());
}

// ---------------------------------------------------------------------------
// 8. DashboardSnapshotToJson
// ---------------------------------------------------------------------------

TEST(SnapshotTypes, DashboardSnapshotToJson)
{
    DashboardSnapshot snap;
    snap.timestamp_ms = 1700000000000;

    // Populate minimal data in each panel.
    snap.order_book.symbol = "BTC_USDT";
    snap.order_book.bids.push_back({50000.0, 1.0});

    snap.kline.symbol = "BTC_USDT";

    snap.positions.portfolio.openPositionCount = 2;

    snap.metrics.available = false;

    snap.ai.available = false;

    strategy::StrategySnapshot strat;
    strat.name = "MomentumScalper";
    strat.id = "momentum_scalper_BTC_USDT";
    strat.symbol = "BTC_USDT";
    strat.enabled = true;
    strat.running = true;
    strat.poll_interval_ms = 500;
    snap.strategies.strategies.push_back(strat);

    snap.risk.trading_halted = false;

    nlohmann::json j = snap;

    // Verify top-level keys exist.
    EXPECT_TRUE(j.contains("timestamp_ms"));
    EXPECT_TRUE(j.contains("order_book"));
    EXPECT_TRUE(j.contains("kline"));
    EXPECT_TRUE(j.contains("positions"));
    EXPECT_TRUE(j.contains("orders"));
    EXPECT_TRUE(j.contains("metrics"));
    EXPECT_TRUE(j.contains("ai"));
    EXPECT_TRUE(j.contains("strategies"));
    EXPECT_TRUE(j.contains("risk"));

    // Verify timestamp.
    EXPECT_EQ(1700000000000, j["timestamp_ms"].get<std::int64_t>());

    // Verify nested panels have expected content.
    EXPECT_EQ("BTC_USDT", j["order_book"]["symbol"].get<std::string>());
    EXPECT_EQ(2, j["positions"]["portfolio"]["openPositionCount"].get<int>());

    // Verify strategies array.
    ASSERT_TRUE(j["strategies"]["strategies"].is_array());
    ASSERT_EQ(1u, j["strategies"]["strategies"].size());
    EXPECT_EQ("MomentumScalper", j["strategies"]["strategies"][0]["name"].get<std::string>());

    // Verify risk panel.
    EXPECT_FALSE(j["risk"]["trading_halted"].get<bool>());
}
