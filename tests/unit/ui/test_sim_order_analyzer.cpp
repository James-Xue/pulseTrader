// test_sim_order_analyzer.cpp — unit tests for the post-trade analyzer.
//
// Builds order/stop fixtures in the SQLite store and verifies the report.

#include "SimOrderAnalyzer.hpp"
#include "SimOrderDb.hpp"

#include <QTemporaryDir>
#include <QSqlDatabase>

#include <gtest/gtest.h>

using namespace pulse::ui;

namespace
{

class SimOrderAnalyzerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (false == QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        {
            GTEST_SKIP() << "QSQLITE driver not available in this test tree";
        }
        ASSERT_TRUE(m_dir.isValid());
        ASSERT_TRUE(SimOrderDb::init(m_dir.filePath(QStringLiteral("orders_test.db"))));
    }

    void TearDown() override
    {
        QSqlDatabase::removeDatabase(QStringLiteral("sim_orders"));
    }

    QTemporaryDir m_dir;

    // Insert one order and close it (with optional trailing moves).
    void addClosedOrder(const QString &id, const QString &side,
                        double entry, double tp, double sl, double exitPrice,
                        int trailingMoves = 0)
    {
        OrderRecord r;
        r.orderId = id;
        r.apiKey = QStringLiteral("keyA");
        r.side = side;
        r.symbol = QStringLiteral("XAUUSD");
        r.volumeLots = 0.01;
        r.entryPrice = entry;
        r.takeProfit = tp;
        r.stopLoss = sl;
        r.leverage = 100.0;
        r.marginUsdt = 4.0;
        r.feeUsdt = 0.06;
        r.status = QStringLiteral("open");
        r.openedAt = QStringLiteral("2026-08-31 09:00:00");
        ASSERT_TRUE(SimOrderDb::insertOrder(r));

        // Trailing moves: each raises the stops by 1 point.
        for (int i = 1; i <= trailingMoves; ++i)
        {
            StopUpdateRecord u;
            u.orderId = id;
            u.updatedAt = QStringLiteral("2026-08-31 09:%1:00").arg(5 + i);
            u.takeProfit = tp + i;
            u.stopLoss = sl + i;
            u.price = entry + i;
            u.reason = QStringLiteral("trailing");
            ASSERT_TRUE(SimOrderDb::insertStopUpdate(u));
        }

        // Final stops for the close (last trailing values).
        const double finalTp = tp + trailingMoves;
        const double finalSl = sl + trailingMoves;
        const bool isLong = (side != QStringLiteral("short"));
        const double pnl = isLong
                               ? (exitPrice - entry) * 0.01 * 100.0
                               : (entry - exitPrice) * 0.01 * 100.0;
        ASSERT_TRUE(SimOrderDb::closeOrder(id, exitPrice, pnl,
                                           QStringLiteral("2026-08-31 10:00:00")));
        Q_UNUSED(finalTp);
        Q_UNUSED(finalSl);
    }
};

} // namespace

TEST_F(SimOrderAnalyzerTest, EmptyKeyGivesEmptyReport)
{
    const auto report = SimOrderAnalyzer::analyze(QStringLiteral("nokey"));
    EXPECT_EQ(0, report.totalOrders);
}

TEST_F(SimOrderAnalyzerTest, WinRateAndPnlAggregates)
{
    // TP close at 4005 → pnl +5.0 (win).
    addClosedOrder(QStringLiteral("SIM-A-0"), QStringLiteral("long"),
                   4000.0, 4005.0, 3995.0, 4005.0);
    // SL close at 3950 → pnl −50.0 (loss).
    addClosedOrder(QStringLiteral("SIM-A-1"), QStringLiteral("long"),
                   4000.0, 4005.0, 3950.0, 3950.0);

    const auto report = SimOrderAnalyzer::analyze(QStringLiteral("keyA"));
    EXPECT_EQ(2, report.totalOrders);
    EXPECT_EQ(2, report.closedOrders);
    EXPECT_EQ(1, report.wins);
    EXPECT_EQ(1, report.losses);
    EXPECT_DOUBLE_EQ(-45.0, report.totalPnl);
    EXPECT_EQ(1, report.tpCloses);
    EXPECT_EQ(1, report.slCloses);
    EXPECT_EQ(2, report.longCount);
}

TEST_F(SimOrderAnalyzerTest, TrailingMovesReflectedInSummary)
{
    // Two trailing moves: TP 4005→4007, SL 3995→3997; final close at TP 4007.
    addClosedOrder(QStringLiteral("SIM-B-0"), QStringLiteral("long"),
                   4000.0, 4005.0, 3995.0, 4007.0, /*trailingMoves=*/2);

    const auto report = SimOrderAnalyzer::analyze(QStringLiteral("keyA"));
    ASSERT_EQ(1, report.orders.size());
    const auto &o = report.orders[0];
    EXPECT_EQ(2, o.stopMoves);
    EXPECT_DOUBLE_EQ(4005.0, o.tpInitial);
    EXPECT_DOUBLE_EQ(4007.0, o.tpFinal);
    EXPECT_DOUBLE_EQ(3997.0, o.slFinal);
    EXPECT_EQ(QStringLiteral("TP"), o.closeReason);   // exit 4007 == final TP.
    EXPECT_DOUBLE_EQ(7.0, o.realizedPnl);             // (4007−4000)×0.01×100.
}

TEST_F(SimOrderAnalyzerTest, LongShortSplit)
{
    addClosedOrder(QStringLiteral("SIM-C-0"), QStringLiteral("long"),
                   4000.0, 4005.0, 3995.0, 4005.0);   // +5.0
    addClosedOrder(QStringLiteral("SIM-C-1"), QStringLiteral("short"),
                   4000.0, 3995.0, 4005.0, 3995.0);   // +5.0 short

    const auto report = SimOrderAnalyzer::analyze(QStringLiteral("keyA"));
    EXPECT_EQ(1, report.longCount);
    EXPECT_DOUBLE_EQ(5.0, report.longPnl);
    EXPECT_EQ(1, report.shortCount);
    EXPECT_DOUBLE_EQ(5.0, report.shortPnl);
    EXPECT_EQ(2, report.tpCloses);
}

TEST_F(SimOrderAnalyzerTest, OpenOrdersNotCountedInStats)
{
    OrderRecord r;
    r.orderId = QStringLiteral("SIM-D-0");
    r.apiKey = QStringLiteral("keyA");
    r.side = QStringLiteral("long");
    r.symbol = QStringLiteral("XAUUSD");
    r.volumeLots = 0.01;
    r.entryPrice = 4000.0;
    r.takeProfit = 4005.0;
    r.stopLoss = 3995.0;
    r.leverage = 100.0;
    r.marginUsdt = 4.0;
    r.feeUsdt = 0.06;
    r.status = QStringLiteral("open");
    r.openedAt = QStringLiteral("2026-08-31 09:00:00");
    ASSERT_TRUE(SimOrderDb::insertOrder(r));

    const auto report = SimOrderAnalyzer::analyze(QStringLiteral("keyA"));
    EXPECT_EQ(1, report.totalOrders);
    EXPECT_EQ(0, report.closedOrders);
    EXPECT_EQ(1, report.openOrders);
    EXPECT_EQ(0, report.wins + report.losses);
    EXPECT_TRUE(report.orders[0].closeReason.isEmpty());
}
