// test_trade_queries.cpp — Query tests for TradeRecorder (Phase 2)

#include "trade_recorder/trade_recorder.hpp"

#include "execution/execution_report.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace pulse;
using namespace pulse::trade_recorder;
using namespace pulse::execution;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

ExecutionReport make_report(
    const std::string &order_id,
    const std::string &symbol,
    Side side,
    double filled_qty,
    double avg_price)
{
    ExecutionReport r;
    r.order_id = order_id;
    r.client_order_id = "";
    r.symbol = symbol;
    r.side = side;
    r.type = OrderType::Market;
    r.requested_qty = filled_qty;
    r.filled_qty = filled_qty;
    r.avg_fill_price = avg_price;
    r.submit_mid_price = avg_price - 1.0;
    r.slippage_bps = 1.0;
    r.fees = 0.1;
    r.latency = std::chrono::milliseconds(100);
    r.submit_time = std::chrono::system_clock::now();
    r.fill_time = r.submit_time + r.latency;
    r.final_status = OrderStatus::Filled;
    return r;
}

TradeRecorder open_populated_db()
{
    auto result = TradeRecorder::open(":memory:");
    EXPECT_TRUE(ok(result));
    auto &rec = value(result);

    // 5 trades: 3 BTC_USDT wins, 2 ETH_USDT losses.
    EXPECT_TRUE(ok(rec.record_trade(make_report("Q1", "BTC_USDT", Side::Buy, 0.001, 65000), 3.0, "momentum_scalper")));
    EXPECT_TRUE(ok(rec.record_trade(make_report("Q2", "BTC_USDT", Side::Sell, 0.001, 65500), 5.0, "momentum_scalper")));
    EXPECT_TRUE(ok(rec.record_trade(make_report("Q3", "BTC_USDT", Side::Buy, 0.002, 64000), 2.0, "orderbook_scalper")));
    EXPECT_TRUE(ok(rec.record_trade(make_report("Q4", "ETH_USDT", Side::Buy, 0.01, 3500), -1.5, "mean_reversion")));
    EXPECT_TRUE(ok(rec.record_trade(make_report("Q5", "ETH_USDT", Side::Sell, 0.01, 3400), -2.5, "mean_reversion")));

    return std::move(rec);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// get_trades
// ---------------------------------------------------------------------------

TEST(TradeQueries, GetTradesAll)
{
    auto recorder = open_populated_db();
    auto result = recorder.get_trades();
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(5u, value(result).size());
}

TEST(TradeQueries, GetTradesBySymbol)
{
    auto recorder = open_populated_db();
    auto result = recorder.get_trades("BTC_USDT");
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(3u, value(result).size());
}

TEST(TradeQueries, GetTradesByTimeRange)
{
    auto recorder = open_populated_db();

    // All trades were inserted just now, so a wide range returns all.
    auto result = recorder.get_trades("", 0, std::numeric_limits<std::int64_t>::max());
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(5u, value(result).size());

    // A range in the past returns nothing.
    auto empty = recorder.get_trades("", 1000, 2000);
    ASSERT_TRUE(ok(empty));
    EXPECT_EQ(0u, value(empty).size());
}

TEST(TradeQueries, GetTradesBySymbolAndTimeRange)
{
    auto recorder = open_populated_db();
    auto result = recorder.get_trades(
        "ETH_USDT", 0, std::numeric_limits<std::int64_t>::max());
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(2u, value(result).size());
}

TEST(TradeQueries, GetTradesEmptyResult)
{
    auto recorder = open_populated_db();
    auto result = recorder.get_trades("DOGE_USDT");
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(0u, value(result).size());
}

// ---------------------------------------------------------------------------
// get_trades_by_strategy
// ---------------------------------------------------------------------------

TEST(TradeQueries, GetTradesByStrategy)
{
    auto recorder = open_populated_db();
    auto result = recorder.get_trades_by_strategy("momentum_scalper");
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(2u, value(result).size());
}

TEST(TradeQueries, GetTradesByStrategyUnknown)
{
    auto recorder = open_populated_db();
    auto result = recorder.get_trades_by_strategy("nonexistent_strategy");
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(0u, value(result).size());
}

// ---------------------------------------------------------------------------
// get_summary
// ---------------------------------------------------------------------------

TEST(TradeQueries, GetSummaryAll)
{
    auto recorder = open_populated_db();
    auto result = recorder.get_summary();
    ASSERT_TRUE(ok(result));

    const auto &s = value(result);
    EXPECT_EQ(5, s.trade_count);
    EXPECT_NEAR(6.0, s.total_pnl, 0.01);  // 3+5+2-1.5-2.5 = 6.0
    EXPECT_NEAR(0.5, s.total_fees, 0.01);  // 5 * 0.1
    EXPECT_GT(s.total_volume, 0.0);
}

TEST(TradeQueries, GetSummaryTimeRange)
{
    auto recorder = open_populated_db();

    // Wide range returns all.
    auto result = recorder.get_summary(
        0, std::numeric_limits<std::int64_t>::max());
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(5, value(result).trade_count);

    // Past range returns zero.
    auto empty = recorder.get_summary(1000, 2000);
    ASSERT_TRUE(ok(empty));
    EXPECT_EQ(0, value(empty).trade_count);
}

TEST(TradeQueries, GetSummaryWinRate)
{
    auto recorder = open_populated_db();
    auto result = recorder.get_summary();
    ASSERT_TRUE(ok(result));

    // 3 wins (pnl > 0) out of 5 trades = 0.6.
    EXPECT_NEAR(0.6, value(result).win_rate, 0.01);
}

// ---------------------------------------------------------------------------
// get_daily_pnl
// ---------------------------------------------------------------------------

TEST(TradeQueries, GetDailyPnl)
{
    auto recorder = open_populated_db();

    // Today's PnL — all trades were inserted just now.
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    auto result = recorder.get_daily_pnl(now_ns);
    ASSERT_TRUE(ok(result));
    EXPECT_NEAR(6.0, value(result), 0.01);
}

TEST(TradeQueries, GetDailyPnlEmptyDay)
{
    auto recorder = open_populated_db();

    // A day far in the past — no trades.
    auto result = recorder.get_daily_pnl(0);
    ASSERT_TRUE(ok(result));
    EXPECT_DOUBLE_EQ(0.0, value(result));
}
