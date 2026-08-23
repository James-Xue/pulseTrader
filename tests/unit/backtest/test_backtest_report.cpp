// test_backtest_report.cpp — BacktestReport formatting + JSON export.

#include "backtest/BacktestReport.hpp"

#include <gtest/gtest.h>

namespace pulse::backtest::test
{

namespace
{

BacktestOptions makeOpts()
{
    BacktestOptions opts;
    opts.strategy_name = "ema_resonance_scalper";
    opts.symbol = "ETH_USDT";
    opts.market_type = MarketType::Futures;
    opts.from_ms = 1'700'000'000'000LL;
    opts.to_ms = 1'700'360'000'000LL;
    opts.order_quantity = 20.0;
    opts.quanto_multiplier = 0.01;
    opts.taker_fee_rate = 0.0005;
    return opts;
}

/// Scripted stats: 3 trades (2W/1L), a few signals, a simple equity curve.
BacktestStats makeStats()
{
    BacktestStats s;
    s.signal_count = 4;
    s.entry_signal_count = 3;
    s.ignored_signal_count = 1;
    s.trade_count = 3;
    s.open_at_end = 1;
    s.gross_profit = 4.0;
    s.gross_loss = -2.0;
    s.total_fees = 0.6;
    s.net_pnl = 1.4;
    s.win_rate = 2.0 / 3.0;
    s.profit_factor = 2.0;
    s.max_drawdown = 1.0;
    s.max_drawdown_pct = 4.2;
    s.avg_win = 2.0;
    s.avg_loss = -2.0;
    s.largest_win = 3.0;
    s.largest_loss = -2.0;

    BacktestTrade t;
    t.position_id = "bt_ETH_USDT_0";
    t.side = Side::Buy;
    t.quantity = 20.0;
    t.quanto_multiplier = 0.01;
    t.entry_price = 2412.92;
    t.exit_price = 2392.71;
    t.entry_open_ms = 1'700'000'000'000LL;
    t.exit_open_ms = 1'700'060'000'000LL;
    t.entry_fee = 0.2;
    t.exit_fee = 0.2;
    t.pnl = -4.04;
    t.net_pnl = -4.44;
    t.return_pct = -0.92;
    s.trades.push_back(t);

    EquityPoint pt;
    pt.candle_open_ms = 1'700'000'000'000LL;
    pt.equity = 0.0;
    s.equity_curve.push_back(pt);
    pt.candle_open_ms = 1'700'060'000'000LL;
    pt.equity = 2.5;
    s.equity_curve.push_back(pt);
    return s;
}

KlineLoadStats makeLoad()
{
    KlineLoadStats l;
    l.rows_sqlite = 342;
    l.rows_api = 25;
    l.rows_total = 367;
    l.missing_range_count = 1;
    l.warnings = { "API fetch failed for [1, 2]: boom" };
    return l;
}

} // anonymous namespace

TEST(BacktestReportTest, FormatTable_ContainsKeyFields)
{
    const auto report = BacktestReport(makeOpts(), makeStats(), makeLoad(), {}, 202, 367);
    const std::string text = report.formatTable();

    EXPECT_NE(std::string::npos, text.find("ema_resonance_scalper / ETH_USDT (futures)"));
    EXPECT_NE(std::string::npos, text.find("367 candles (sqlite 342 + api 25)"));
    EXPECT_NE(std::string::npos, text.find("warmup"));
    EXPECT_NE(std::string::npos, text.find("Signals   : 4 (entries 3, ignored 1)"));
    EXPECT_NE(std::string::npos, text.find("Trades    : 3 closed, 1 open at end"));
    EXPECT_NE(std::string::npos, text.find("Net PnL"));
    EXPECT_NE(std::string::npos, text.find("Win rate"));
    EXPECT_NE(std::string::npos, text.find("Profit factor"));
    EXPECT_NE(std::string::npos, text.find("Max drawdown"));
    EXPECT_NE(std::string::npos, text.find("Largest win / loss"));
    EXPECT_NE(std::string::npos, text.find("2412.92"));
    EXPECT_NE(std::string::npos, text.find("2392.71"));
    EXPECT_NE(std::string::npos, text.find("warn: API fetch failed"));
}

TEST(BacktestReportTest, FormatTable_NumericValues)
{
    const auto report = BacktestReport(makeOpts(), makeStats(), makeLoad(), {}, 202, 367);
    const std::string text = report.formatTable();

    // Net PnL = 1.4 with gross +4.0 / -2.0 and fees 0.6.
    EXPECT_NE(std::string::npos, text.find("+1.40 USDT (gross +4.00 / -2.00, fees -0.60)"));
    // Win rate 66.7% (2/3).
    EXPECT_NE(std::string::npos, text.find("66.7% (2/3)"));
    // Profit factor 2.00.
    EXPECT_NE(std::string::npos, text.find("Profit factor : 2.00"));
}

TEST(BacktestReportTest, ToJson_AllSectionsPresent)
{
    BacktestSignal sig;
    sig.candle_open_ms = 1'700'000'000'000LL;
    sig.type = strategy::SignalType::Buy;
    sig.confidence = 0.9;
    sig.price = 2412.92;
    sig.reason = "bull aligned";
    sig.indicators = { { "resonance", "bull_aligned" } };

    const auto report = BacktestReport(
        makeOpts(), makeStats(), makeLoad(), { sig }, 202, 367);
    const auto j = report.toJson();

    EXPECT_EQ("ema_resonance_scalper", j["strategy"].get<std::string>());
    EXPECT_EQ("ETH_USDT", j["symbol"].get<std::string>());
    EXPECT_EQ("futures", j["market_type"].get<std::string>());

    EXPECT_EQ(1'700'000'000'000LL, j["range"]["from_ms"].get<std::int64_t>());
    EXPECT_EQ(367u, j["data"]["rows_total"].get<std::size_t>());
    EXPECT_EQ(1, j["data"]["missing_ranges"].get<int>());
    ASSERT_TRUE(j["data"]["warnings"].is_array());
    EXPECT_EQ(1u, j["data"]["warnings"].size());

    EXPECT_EQ(202u, j["warmup_candles"].get<std::size_t>());
    EXPECT_EQ(367u, j["candles_fed"].get<std::size_t>());

    // Signals.
    ASSERT_TRUE(j["signals"].is_array());
    ASSERT_EQ(1u, j["signals"].size());
    EXPECT_EQ("Buy", j["signals"][0]["type"].get<std::string>());
    EXPECT_DOUBLE_EQ(0.9, j["signals"][0]["confidence"].get<double>());
    EXPECT_EQ("bull_aligned",
              j["signals"][0]["indicators"]["resonance"].get<std::string>());

    // Stats.
    EXPECT_EQ(4, j["stats"]["signal_count"].get<int>());
    EXPECT_DOUBLE_EQ(1.4, j["stats"]["net_pnl"].get<double>());
    EXPECT_DOUBLE_EQ(2.0 / 3.0, j["stats"]["win_rate"].get<double>());

    // Trades.
    ASSERT_TRUE(j["trades"].is_array());
    ASSERT_EQ(1u, j["trades"].size());
    EXPECT_EQ("Buy", j["trades"][0]["side"].get<std::string>());
    EXPECT_DOUBLE_EQ(2412.92, j["trades"][0]["entry_price"].get<double>());
    EXPECT_DOUBLE_EQ(-4.44, j["trades"][0]["net_pnl"].get<double>());

    // Equity curve.
    ASSERT_TRUE(j["equity_curve"].is_array());
    ASSERT_EQ(2u, j["equity_curve"].size());
    EXPECT_DOUBLE_EQ(2.5, j["equity_curve"][1]["equity"].get<double>());
}

TEST(BacktestReportTest, ToJson_EmptyTradeList)
{
    BacktestOptions opts = makeOpts();
    BacktestStats stats;
    KlineLoadStats load;
    const auto report = BacktestReport(opts, stats, load, {}, 0, 0);
    const auto j = report.toJson();
    EXPECT_TRUE(j["trades"].empty());
    EXPECT_TRUE(j["signals"].empty());
    EXPECT_TRUE(j["equity_curve"].empty());
    EXPECT_DOUBLE_EQ(0.0, j["stats"]["net_pnl"].get<double>());
}

} // namespace pulse::backtest::test
