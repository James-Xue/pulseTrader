// test_backtest_account.cpp — BacktestAccount fill simulation (pure memory).

#include "backtest/BacktestAccount.hpp"

#include <gtest/gtest.h>

namespace pulse::backtest::test
{

namespace
{

BacktestOptions makeOpts(CloseMode mode = CloseMode::Flip)
{
    BacktestOptions opts;
    opts.symbol = "ETH_USDT";
    opts.market_type = MarketType::Futures;
    opts.order_quantity = 20.0;        // contracts
    opts.quanto_multiplier = 0.01;     // ETH_USDT
    opts.taker_fee_rate = 0.0005;      // futures taker
    opts.close_mode = mode;
    return opts;
}

strategy::TradingSignal makeSignal(strategy::SignalType type, double price)
{
    strategy::TradingSignal sig;
    sig.type = type;
    sig.price = price;
    sig.confidence = 1.0;
    sig.symbol = "ETH_USDT";
    return sig;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PnL formulas (mirror PositionManager::closePosition)
// ---------------------------------------------------------------------------

TEST(BacktestAccountTest, LongRoundTrip_PnlFormula)
{
    BacktestAccount account(makeOpts());
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
    account.onSignal(makeSignal(strategy::SignalType::Sell, 2050.0), 1'060'000);

    // The Sell closes the long AND (Flip mode) opens a short — exactly one
    // trade at this point.
    ASSERT_EQ(1u, account.trades().size());
    const auto &t = account.trades()[0];

    // Gross = (2050-2000) × 20 × 0.01 = 10.0.
    EXPECT_DOUBLE_EQ(10.0, t.pnl);
    // Fees: entry 2000×20×0.01×0.0005 = 0.2, exit 2050×... = 0.205.
    EXPECT_DOUBLE_EQ(0.2, t.entry_fee);
    EXPECT_DOUBLE_EQ(0.205, t.exit_fee);
    EXPECT_DOUBLE_EQ(10.0 - 0.2 - 0.205, t.net_pnl);
    EXPECT_EQ(Side::Buy, t.side);
    EXPECT_EQ(1'000'000LL, t.entry_open_ms);
    EXPECT_EQ(1'060'000LL, t.exit_open_ms);

    // The window-end closeAll flattens the leftover short → 2 trades total.
    account.closeAll(1'120'000, 2050.0);
    ASSERT_EQ(2u, account.trades().size());
    EXPECT_EQ(Side::Sell, account.trades()[1].side);
    EXPECT_EQ(1'120'000LL, account.trades()[1].exit_open_ms);
}

TEST(BacktestAccountTest, ShortRoundTrip_PnlFormula)
{
    BacktestAccount account(makeOpts());
    account.onSignal(makeSignal(strategy::SignalType::Sell, 2100.0), 1'000'000);
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2050.0), 1'060'000);

    ASSERT_EQ(1u, account.trades().size());
    const auto &t = account.trades()[0];
    // Gross = (2100-2050) × 20 × 0.01 = 10.0.
    EXPECT_DOUBLE_EQ(10.0, t.pnl);
    EXPECT_EQ(Side::Sell, t.side);
}

TEST(BacktestAccountTest, SpotUsesUnitQuantoAndDefaultFee)
{
    BacktestOptions opts = makeOpts();
    opts.market_type = MarketType::Spot;
    opts.quanto_multiplier = 1.0;
    opts.order_quantity = 0.1;  // 0.1 ETH
    opts.taker_fee_rate = 0.0;  // exercise the spot default (0.001)
    BacktestAccount account(opts);

    account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
    account.onSignal(makeSignal(strategy::SignalType::Sell, 2100.0), 1'060'000);

    // Gross = 100 × 0.1 × 1.0 = 10.0; spot default fee rate 0.001.
    EXPECT_DOUBLE_EQ(10.0, account.trades()[0].pnl);
    EXPECT_DOUBLE_EQ(0.1 * 2000.0 * 0.001, account.trades()[0].entry_fee);
    EXPECT_DOUBLE_EQ(0.1 * 2100.0 * 0.001, account.trades()[0].exit_fee);
}

// ---------------------------------------------------------------------------
// Flip-mode position semantics
// ---------------------------------------------------------------------------

TEST(BacktestAccountTest, Flip_SameDirectionIgnored)
{
    BacktestAccount account(makeOpts());
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2010.0), 1'060'000); // ignored

    EXPECT_EQ(1, account.entrySignalCount());
    EXPECT_EQ(1, account.ignoredSignalCount());
    EXPECT_TRUE(account.hasPosition());
    EXPECT_EQ(0u, account.trades().size());
}

TEST(BacktestAccountTest, Flip_OppositeClosesThenOpens)
{
    BacktestAccount account(makeOpts());
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
    account.onSignal(makeSignal(strategy::SignalType::Sell, 1980.0), 1'060'000);

    // One closed trade + a new short position open.
    ASSERT_EQ(1u, account.trades().size());
    EXPECT_EQ(Side::Buy, account.trades()[0].side);
    // Gross = (1980-2000) × 20 × 0.01 = -4.0; net after both fees.
    EXPECT_DOUBLE_EQ(-4.0, account.trades()[0].pnl);
    EXPECT_DOUBLE_EQ(-4.0 - 0.2 - 0.198, account.trades()[0].net_pnl);
    EXPECT_TRUE(account.hasPosition());
    EXPECT_EQ(2, account.entrySignalCount());
    EXPECT_EQ(0, account.ignoredSignalCount());
}

TEST(BacktestAccountTest, Flip_CloseAllAtWindowEnd)
{
    BacktestAccount account(makeOpts());
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
    account.closeAll(1'120'000, 2030.0);

    ASSERT_EQ(1u, account.trades().size());
    EXPECT_EQ(1'120'000LL, account.trades()[0].exit_open_ms);
    EXPECT_DOUBLE_EQ(2030.0, account.trades()[0].exit_price);
    EXPECT_FALSE(account.hasPosition());
}

TEST(BacktestAccountTest, FlatSignalNotCounted)
{
    BacktestAccount account(makeOpts());
    account.onSignal(makeSignal(strategy::SignalType::Flat, 2000.0), 1'000'000);
    EXPECT_EQ(0, account.entrySignalCount());
    EXPECT_EQ(0, account.ignoredSignalCount());
    EXPECT_FALSE(account.hasPosition());
}

// ---------------------------------------------------------------------------
// Equity curve + stats
// ---------------------------------------------------------------------------

TEST(BacktestAccountTest, EquityCurve_SamplesRealizedAndUnrealized)
{
    BacktestAccount account(makeOpts());
    account.sampleEquity(1'000'000, 2000.0); // flat, 0
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
    account.sampleEquity(1'060'000, 2025.0); // +25×20×0.01 = +5.0 unrealized
    account.onSignal(makeSignal(strategy::SignalType::Sell, 2025.0), 1'060'000);
    account.sampleEquity(1'120'000, 2025.0); // realized

    const auto &curve = account.equityCurve();
    ASSERT_EQ(3u, curve.size());
    EXPECT_DOUBLE_EQ(0.0, curve[0].equity);
    EXPECT_DOUBLE_EQ(5.0, curve[1].equity);
    EXPECT_DOUBLE_EQ(5.0 - 0.2 - 0.2025, curve[2].equity);
}

TEST(BacktestAccountTest, Stats_WinRateAndProfitFactor)
{
    BacktestOptions opts = makeOpts();
    opts.taker_fee_rate = -1.0; // disable fees to isolate PnL math
    BacktestAccount account(opts);

    // Flip chain (qty 20 contracts × quanto 0.01 → each price unit = 0.2):
    //   Buy@2000 → Sell@2010 (close long +10u = +2.0; open short@2010)
    //   Buy@2020 (close short −10u = −2.0; open long@2020)
    //   closeAll@2030 (close long +10u = +2.0)
    // 3 trades: 2W/1L.
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
    account.onSignal(makeSignal(strategy::SignalType::Sell, 2010.0), 1'060'000);
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2020.0), 1'120'000);
    account.closeAll(1'180'000, 2030.0);

    const auto s = account.stats();
    EXPECT_EQ(3, s.trade_count);
    EXPECT_DOUBLE_EQ(2.0 / 3.0, s.win_rate);
    EXPECT_DOUBLE_EQ(4.0, s.gross_profit);
    EXPECT_DOUBLE_EQ(-2.0, s.gross_loss);
    EXPECT_DOUBLE_EQ(2.0, s.profit_factor);
    EXPECT_DOUBLE_EQ(2.0, s.net_pnl);
    EXPECT_DOUBLE_EQ(2.0, s.avg_win);
    EXPECT_DOUBLE_EQ(-2.0, s.avg_loss);
    EXPECT_DOUBLE_EQ(2.0, s.largest_win);
    EXPECT_DOUBLE_EQ(-2.0, s.largest_loss);
}

TEST(BacktestAccountTest, Stats_MaxDrawdownFromEquityCurve)
{
    BacktestOptions opts = makeOpts();
    opts.taker_fee_rate = -1.0; // disable fees to isolate PnL math

    // Scenario A: all-negative equity → absolute drawdown still reported,
    // pct stays 0 (peak never non-zero).
    {
        BacktestAccount account(opts);
        account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
        account.sampleEquity(1'060'000, 2000.0); // 0
        account.onSignal(makeSignal(strategy::SignalType::Sell, 1995.0), 1'060'000);
        account.sampleEquity(1'120'000, 1995.0); // -1.0 (5u × 20 × 0.01)
        account.closeAll(1'120'000, 1995.0);

        const auto s = account.stats();
        EXPECT_DOUBLE_EQ(1.0, s.max_drawdown);
        EXPECT_DOUBLE_EQ(0.0, s.max_drawdown_pct);
    }

    // Scenario B: equity [0, 5, 7, 5] → peak 7, dd = 2, pct = 2/7.
    {
        BacktestAccount account(opts);
        account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
        account.sampleEquity(1'000'000, 2025.0); // unrealized +5
        account.onSignal(makeSignal(strategy::SignalType::Sell, 2025.0), 1'000'000);
        account.sampleEquity(1'060'000, 2015.0); // realized 5 + short +2 → 7
        account.onSignal(makeSignal(strategy::SignalType::Buy, 2015.0), 1'060'000);
        account.sampleEquity(1'120'000, 2005.0); // 7 + long -2 → 5
        account.closeAll(1'120'000, 2005.0);

        const auto s = account.stats();
        EXPECT_DOUBLE_EQ(2.0, s.max_drawdown);
        EXPECT_DOUBLE_EQ(2.0 / 7.0 * 100.0, s.max_drawdown_pct);
    }
}

TEST(BacktestAccountTest, Independent_OnePerDirection)
{
    BacktestAccount account(makeOpts(CloseMode::Independent));
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);  // open long
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2010.0), 1'060'000);  // ignored (long open)
    account.onSignal(makeSignal(strategy::SignalType::Sell, 2010.0), 1'120'000); // closes long, opens short
    account.onSignal(makeSignal(strategy::SignalType::Sell, 1990.0), 1'180'000); // ignored (short open)

    // The Sell@2010 closed the long → 1 trade so far.
    ASSERT_EQ(1u, account.trades().size());
    EXPECT_EQ(Side::Buy, account.trades()[0].side);
    // Short still open (Independent keeps directions separate).
    EXPECT_TRUE(account.hasPosition());
    EXPECT_EQ(2, account.entrySignalCount());
    EXPECT_EQ(2, account.ignoredSignalCount());

    account.closeAll(1'240'000, 2000.0);
    EXPECT_EQ(2u, account.trades().size());
    EXPECT_EQ(Side::Sell, account.trades()[1].side);
}

TEST(BacktestAccountTest, ZeroQuantityCannotTrade)
{
    BacktestOptions opts = makeOpts();
    opts.order_quantity = 0.0;
    BacktestAccount account(opts);
    account.onSignal(makeSignal(strategy::SignalType::Buy, 2000.0), 1'000'000);
    EXPECT_FALSE(account.hasPosition());
    EXPECT_EQ(0, account.entrySignalCount());
    EXPECT_EQ(1, account.ignoredSignalCount());
}

} // namespace pulse::backtest::test
