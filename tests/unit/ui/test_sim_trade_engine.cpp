// test_sim_trade_engine.cpp — unit tests for the paper-trading engine.
//
// Mirrors the real CFD model: margin = notional/leverage, BUY-side
// commission, TP/SL settlement at the trigger price. The store writes to
// data/sim_account.json under the test working dir; each test cleans up.

#include "SimTradeEngine.hpp"

#include <QFile>

#include <gtest/gtest.h>

using namespace pulse::ui;

namespace
{

constexpr std::int64_t kNow = 1'700'000'000'000LL;
constexpr double kOzPerLot = SimTradeEngine::kOzPerLot;

class SimTradeEngineTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        QFile::remove(SimAccountStore::filePath());
        EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 10'000.0));
    }

    void TearDown() override
    {
        QFile::remove(SimAccountStore::filePath());
    }
};

// Open one 0.01-lot long at 4000 with 100x leverage and a wide TP/SL.
SimTradeEngine::OpenResult openOne(const QString &key = QStringLiteral("keyA"),
                                   double lots = 0.01, double price = 4000.0,
                                   double leverage = 100.0)
{
    return SimTradeEngine::openLong(key, lots, price, price + 100.0, price - 50.0,
                                    leverage, kNow);
}

} // namespace

TEST_F(SimTradeEngineTest, OpenDeductsMarginAndFee)
{
    const auto open = openOne();
    ASSERT_TRUE(open.ok) << open.error.toStdString();
    // Margin = 0.01×100×4000 / 100 = 40; fee = 0.01×6 = 0.06.
    EXPECT_DOUBLE_EQ(40.0, open.marginUsdt);
    EXPECT_DOUBLE_EQ(0.06, open.feeUsdt);
    EXPECT_DOUBLE_EQ(10000.0 - 40.0 - 0.06,
                     SimAccountStore::load(QStringLiteral("keyA"))->balanceUsdt);

    const auto positions = SimTradeEngine::positions(QStringLiteral("keyA"));
    ASSERT_EQ(1, positions.size());
    EXPECT_EQ(0.01, positions[0].volumeLots);
    EXPECT_EQ(4000.0, positions[0].entryPrice);
    EXPECT_EQ(40.0, positions[0].marginUsdt);
}

TEST_F(SimTradeEngineTest, LeverageScalesMargin)
{
    const auto low = openOne(QStringLiteral("keyA"), 0.01, 4000.0, 10.0);
    ASSERT_TRUE(low.ok);
    EXPECT_DOUBLE_EQ(400.0, low.marginUsdt);  // notional 4000 / 10x

    // Open a second position on the same account with 500x.
    const auto high = SimTradeEngine::openLong(
        QStringLiteral("keyA"), 0.01, 4000.0, 4100.0, 3950.0, 500.0, kNow + 1);
    ASSERT_TRUE(high.ok);
    EXPECT_DOUBLE_EQ(8.0, high.marginUsdt);   // notional 4000 / 500x
}

TEST_F(SimTradeEngineTest, OpenRejectedWithoutAccount)
{
    const auto open = SimTradeEngine::openLong(
        QStringLiteral("nokey"), 0.01, 4000.0, 4100.0, 3950.0, 100.0, kNow);
    EXPECT_FALSE(open.ok);
    EXPECT_TRUE(open.error.contains(QStringLiteral("未设置模拟账户")));
}

TEST_F(SimTradeEngineTest, OpenRejectedWithoutTpSl)
{
    // Real CFD rule: an entry order MUST carry both TP and SL at open —
    // there is no "open first, set stops later" in the real workflow.
    const auto noTp = SimTradeEngine::openLong(
        QStringLiteral("keyA"), 0.01, 4000.0, 0.0, 3950.0, 100.0, kNow);
    EXPECT_FALSE(noTp.ok);
    EXPECT_TRUE(noTp.error.contains(QStringLiteral("止盈止损")));

    const auto noSl = SimTradeEngine::openLong(
        QStringLiteral("keyA"), 0.01, 4000.0, 4100.0, 0.0, 100.0, kNow);
    EXPECT_FALSE(noSl.ok);
    EXPECT_TRUE(noSl.error.contains(QStringLiteral("止盈止损")));
    EXPECT_TRUE(SimTradeEngine::positions(QStringLiteral("keyA")).empty());
}

TEST_F(SimTradeEngineTest, OpenRejectsWrongSidedStops)
{
    // A long needs TP > entry > SL — wrong-sided stops are rejected.
    const auto badTp = SimTradeEngine::openLong(
        QStringLiteral("keyA"), 0.01, 4000.0, 3900.0, 3950.0, 100.0, kNow);
    EXPECT_FALSE(badTp.ok);
    EXPECT_TRUE(badTp.error.contains(QStringLiteral("止盈必须高于开仓价")));

    const auto badSl = SimTradeEngine::openLong(
        QStringLiteral("keyA"), 0.01, 4000.0, 4100.0, 4050.0, 100.0, kNow);
    EXPECT_FALSE(badSl.ok);
    EXPECT_TRUE(badSl.error.contains(QStringLiteral("止损必须低于开仓价")));
    EXPECT_TRUE(SimTradeEngine::positions(QStringLiteral("keyA")).empty());
}

TEST_F(SimTradeEngineTest, OpenRejectedOnInsufficientBalance)
{
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 1.0));  // tiny balance
    const auto open = openOne();
    EXPECT_FALSE(open.ok);
    EXPECT_TRUE(open.error.contains(QStringLiteral("余额不足")));
    EXPECT_DOUBLE_EQ(1.0, SimAccountStore::load(QStringLiteral("keyA"))->balanceUsdt);
}

TEST_F(SimTradeEngineTest, TpHitSettlesProfit)
{
    ASSERT_TRUE(openOne().ok);
    const auto closes = SimTradeEngine::checkAndClose(QStringLiteral("keyA"), 4100.0, kNow + 1);
    ASSERT_EQ(1, closes.size());
    EXPECT_EQ(QStringLiteral("TP"), closes[0].reason);
    EXPECT_DOUBLE_EQ(4100.0, closes[0].exitPrice);
    // PnL = (4100−4000) × 0.01 × 100 = 100; margin 40 released.
    EXPECT_DOUBLE_EQ(100.0, closes[0].realizedPnl);
    EXPECT_DOUBLE_EQ(40.0, closes[0].releasedMargin);
    // Balance = 10000 − 40.06 (open) + 40 (margin) + 100 (pnl).
    EXPECT_DOUBLE_EQ(10000.0 - 40.06 + 40.0 + 100.0,
                     SimAccountStore::load(QStringLiteral("keyA"))->balanceUsdt);
    EXPECT_TRUE(SimTradeEngine::positions(QStringLiteral("keyA")).empty());
}

TEST_F(SimTradeEngineTest, SlHitSettlesLoss)
{
    ASSERT_TRUE(openOne().ok);
    const auto closes = SimTradeEngine::checkAndClose(QStringLiteral("keyA"), 3950.0, kNow + 1);
    ASSERT_EQ(1, closes.size());
    EXPECT_EQ(QStringLiteral("SL"), closes[0].reason);
    EXPECT_DOUBLE_EQ(-50.0, closes[0].realizedPnl);  // (3950−4000)×0.01×100
    // Balance = 10000 − 40.06 + 40 − 50.
    EXPECT_DOUBLE_EQ(10000.0 - 40.06 + 40.0 - 50.0,
                     SimAccountStore::load(QStringLiteral("keyA"))->balanceUsdt);
}

TEST_F(SimTradeEngineTest, PriceInsideRangeKeepsPosition)
{
    ASSERT_TRUE(openOne().ok);
    const auto closes = SimTradeEngine::checkAndClose(QStringLiteral("keyA"), 4020.0, kNow + 1);
    EXPECT_TRUE(closes.empty());
    EXPECT_EQ(1, SimTradeEngine::positions(QStringLiteral("keyA")).size());
}

TEST_F(SimTradeEngineTest, MultiplePositionsCloseIndependently)
{
    ASSERT_TRUE(openOne(QStringLiteral("keyA"), 0.01, 4000.0, 100.0).ok);   // TP 4100
    ASSERT_TRUE(openOne(QStringLiteral("keyA"), 0.02, 4050.0, 100.0).ok);   // TP 4150

    const auto closes = SimTradeEngine::checkAndClose(QStringLiteral("keyA"), 4120.0, kNow + 1);
    ASSERT_EQ(1, closes.size());                       // only the first TP hit
    EXPECT_EQ(QStringLiteral("TP"), closes[0].reason);
    EXPECT_EQ(1, SimTradeEngine::positions(QStringLiteral("keyA")).size());

    const auto closes2 = SimTradeEngine::checkAndClose(QStringLiteral("keyA"), 4160.0, kNow + 2);
    ASSERT_EQ(1, closes2.size());
    EXPECT_TRUE(SimTradeEngine::positions(QStringLiteral("keyA")).empty());
}

TEST_F(SimTradeEngineTest, PositionsPersistAcrossLoad)
{
    ASSERT_TRUE(openOne().ok);
    const auto reloaded = SimAccountStore::load(QStringLiteral("keyA"));
    ASSERT_TRUE(reloaded.has_value());
    ASSERT_EQ(1, reloaded->positions.size());
    EXPECT_DOUBLE_EQ(4000.0, reloaded->positions[0].entryPrice);
    EXPECT_DOUBLE_EQ(40.0, reloaded->positions[0].marginUsdt);
}

// ---------------------------------------------------------------------------
// updateStops — trailing stops move only in the favourable direction
// ---------------------------------------------------------------------------

TEST_F(SimTradeEngineTest, UpdateStopsMovesUpOnRisingMarket)
{
    ASSERT_TRUE(openOne().ok);  // TP 4100, SL 3950.
    // Market moved up: suggestion TP 4150 / SL 3980 → both move up.
    const auto update =
        SimTradeEngine::updateStops(QStringLiteral("keyA"), 4150.0, 3980.0);
    ASSERT_TRUE(update.ok);
    EXPECT_EQ(1, update.updatedCount);
    const auto positions = SimTradeEngine::positions(QStringLiteral("keyA"));
    ASSERT_EQ(1, positions.size());
    EXPECT_DOUBLE_EQ(4150.0, positions[0].takeProfit);
    EXPECT_DOUBLE_EQ(3980.0, positions[0].stopLoss);
}

TEST_F(SimTradeEngineTest, UpdateStopsNeverLowersStops)
{
    ASSERT_TRUE(openOne().ok);  // TP 4100, SL 3950.
    // Suggestion below the current stops → nothing moves (profit is
    // protected, mirroring a real trailing stop).
    const auto update =
        SimTradeEngine::updateStops(QStringLiteral("keyA"), 4080.0, 3930.0);
    ASSERT_TRUE(update.ok);
    EXPECT_EQ(0, update.updatedCount);
    const auto positions = SimTradeEngine::positions(QStringLiteral("keyA"));
    ASSERT_EQ(1, positions.size());
    EXPECT_DOUBLE_EQ(4100.0, positions[0].takeProfit);
    EXPECT_DOUBLE_EQ(3950.0, positions[0].stopLoss);
}

TEST_F(SimTradeEngineTest, UpdateStopsSkipsInvalidSuggestions)
{
    ASSERT_TRUE(openOne().ok);
    const auto update =
        SimTradeEngine::updateStops(QStringLiteral("keyA"), 3900.0, 4100.0);  // TP < SL
    EXPECT_FALSE(update.ok);
    // Position untouched.
    const auto positions = SimTradeEngine::positions(QStringLiteral("keyA"));
    ASSERT_EQ(1, positions.size());
    EXPECT_DOUBLE_EQ(4100.0, positions[0].takeProfit);
    EXPECT_DOUBLE_EQ(3950.0, positions[0].stopLoss);
}

TEST_F(SimTradeEngineTest, UpdateStopsIsNoOpWithoutPositions)
{
    const auto update =
        SimTradeEngine::updateStops(QStringLiteral("keyA"), 4150.0, 3980.0);
    ASSERT_TRUE(update.ok);
    EXPECT_EQ(0, update.updatedCount);
}
