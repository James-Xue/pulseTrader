// test_strategy_summary.cpp — per-strategy aggregation (AI tuning feedback)

#include "trade_recorder/TradeRecorder.hpp"

#include "execution/ExecutionReport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

using namespace pulse;
using namespace pulse::trade_recorder;
using namespace pulse::execution;

namespace
{

ExecutionReport make_report(const std::string &order_id,
                            const std::string &symbol,
                            double filled_qty,
                            double avg_price)
{
    ExecutionReport r;
    r.order_id = order_id;
    r.client_order_id = "";
    r.symbol = symbol;
    r.side = Side::Buy;
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

    // momentum_scalper: 3 trades — 2 wins, 1 loss (win_rate 2/3).
    EXPECT_TRUE(ok(rec.recordTrade(make_report("M1", "BTC_USDT", 0.001, 65000), 3.0,
                                   "momentum_scalper", MarketType::Futures)));
    EXPECT_TRUE(ok(rec.recordTrade(make_report("M2", "BTC_USDT", 0.001, 65500), 5.0,
                                   "momentum_scalper", MarketType::Futures)));
    EXPECT_TRUE(ok(rec.recordTrade(make_report("M3", "ETH_USDT", 0.01, 3500), -1.5,
                                   "momentum_scalper", MarketType::Futures)));
    // mean_reversion: 1 win on CFD.
    EXPECT_TRUE(ok(rec.recordTrade(make_report("R1", "XAUUSD", 0.01, 4400), 2.0,
                                   "mean_reversion_scalper", MarketType::Cfd)));
    return std::move(rec);
}

} // anonymous namespace

TEST(StrategySummary, EmptyDbReturnsEmpty)
{
    auto result = TradeRecorder::open(":memory:");
    ASSERT_TRUE(ok(result));
    auto &rec = value(result);

    const auto summary = rec.getStrategySummary();
    ASSERT_TRUE(ok(summary));
    EXPECT_TRUE(value(summary).empty());
}

TEST(StrategySummary, GroupsByStrategy)
{
    auto rec = open_populated_db();
    const auto summary = rec.getStrategySummary();
    ASSERT_TRUE(ok(summary));
    const auto &rows = value(summary);

    ASSERT_EQ(2u, rows.size());
    const auto mom = std::find_if(rows.begin(), rows.end(),
        [](const StrategyTradeSummary &s) { return s.strategy_name == "momentum_scalper"; });
    const auto mr = std::find_if(rows.begin(), rows.end(),
        [](const StrategyTradeSummary &s) { return s.strategy_name == "mean_reversion_scalper"; });
    ASSERT_NE(rows.end(), mom);
    ASSERT_NE(rows.end(), mr);

    EXPECT_EQ("futures", mom->market_type);
    EXPECT_EQ(3, mom->trade_count);
    EXPECT_NEAR(6.5, mom->total_pnl, 1e-9);      // 3 + 5 - 1.5
    EXPECT_NEAR(2.0 / 3.0, mom->win_rate, 1e-9);
    EXPECT_NEAR(0.3, mom->total_fees, 1e-9);     // 3 × 0.1

    EXPECT_EQ("cfd", mr->market_type);
    EXPECT_EQ(1, mr->trade_count);
    EXPECT_NEAR(2.0, mr->total_pnl, 1e-9);
    EXPECT_NEAR(1.0, mr->win_rate, 1e-9);
}

TEST(StrategySummary, RespectsTimeWindow)
{
    auto rec = open_populated_db();
    // The populated DB uses now()-based timestamps; query only the last
    // second — everything must be included.
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto summary = rec.getStrategySummary(now_ns - 1'000'000'000LL,
                                                now_ns + 1'000'000'000LL);
    ASSERT_TRUE(ok(summary));
    EXPECT_EQ(2u, value(summary).size());

    // A window fully in the past excludes everything.
    const auto empty = rec.getStrategySummary(0, now_ns - 3'600'000'000'000LL);
    ASSERT_TRUE(ok(empty));
    EXPECT_TRUE(value(empty).empty());
}

TEST(StrategySummary, AggregatesFees)
{
    auto rec = open_populated_db();
    const auto summary = rec.getStrategySummary();
    ASSERT_TRUE(ok(summary));
    double total_fees = 0.0;
    for (const auto &row : value(summary))
    {
        total_fees += row.total_fees;
    }
    EXPECT_NEAR(0.4, total_fees, 1e-9); // 4 trades × 0.1
}

TEST(StrategySummary, ClosedDbReturnsError)
{
    auto result = TradeRecorder::open(":memory:");
    ASSERT_TRUE(ok(result));
    auto &rec = value(result);
    rec.close();

    const auto summary = rec.getStrategySummary();
    ASSERT_FALSE(ok(summary));
    EXPECT_EQ(ErrorCode::TradeRecorderNotOpen, error(summary).code);
}
