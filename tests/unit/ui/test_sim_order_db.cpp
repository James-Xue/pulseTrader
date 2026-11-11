// test_sim_order_db.cpp — unit tests for the SQLite order store.
//
// Uses a per-test temporary database file. Skipped when the QSQLITE driver
// is unavailable (plugin not deployed in the test tree).

#include "SimOrderDb.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QSqlDatabase>

#include <gtest/gtest.h>

using namespace pulse::ui;

namespace
{

class SimOrderDbTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (false == QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        {
            GTEST_SKIP() << "QSQLITE driver not available in this test tree";
        }
        ASSERT_TRUE(m_dir.isValid());
        m_dbPath = m_dir.filePath(QStringLiteral("orders_test.db"));
        ASSERT_TRUE(SimOrderDb::init(m_dbPath));
    }

    void TearDown() override
    {
        // Close the shared connection so the temp file can be removed.
        QSqlDatabase::removeDatabase(QStringLiteral("sim_orders"));
    }

    QTemporaryDir m_dir;
    QString m_dbPath;

    static OrderRecord makeOrder(const QString &id, const QString &key,
                                 const QString &side = QStringLiteral("long"))
    {
        OrderRecord r;
        r.orderId = id;
        r.apiKey = key;
        r.side = side;
        r.symbol = QStringLiteral("XAUUSD");
        r.volumeLots = 0.01;
        r.entryPrice = 4000.0;
        r.takeProfit = 4005.0;
        r.stopLoss = 3995.0;
        r.leverage = 100.0;
        r.marginUsdt = 4.0;
        r.feeUsdt = 0.06;
        r.status = QStringLiteral("open");
        r.openedAt = QStringLiteral("2026-08-31 10:00:00");
        return r;
    }
};

} // namespace

TEST_F(SimOrderDbTest, InsertOrderCreatesInitialStopAttachment)
{
    ASSERT_TRUE(SimOrderDb::insertOrder(makeOrder(QStringLiteral("SIM-1-0"), QStringLiteral("keyA"))));

    const auto orders = SimOrderDb::queryOrders(QStringLiteral("keyA"));
    ASSERT_EQ(1, orders.size());
    EXPECT_EQ(QStringLiteral("SIM-1-0"), orders[0].orderId);
    EXPECT_EQ(QStringLiteral("open"), orders[0].status);
    EXPECT_DOUBLE_EQ(4000.0, orders[0].entryPrice);

    const auto stops = SimOrderDb::queryStopUpdates(QStringLiteral("SIM-1-0"));
    ASSERT_EQ(1, stops.size());
    EXPECT_EQ(QStringLiteral("open"), stops[0].reason);
    EXPECT_DOUBLE_EQ(4005.0, stops[0].takeProfit);
    EXPECT_DOUBLE_EQ(3995.0, stops[0].stopLoss);
}

TEST_F(SimOrderDbTest, TrailingUpdatesAppendHistory)
{
    ASSERT_TRUE(SimOrderDb::insertOrder(makeOrder(QStringLiteral("SIM-2-0"), QStringLiteral("keyA"))));

    StopUpdateRecord trailing;
    trailing.orderId = QStringLiteral("SIM-2-0");
    trailing.updatedAt = QStringLiteral("2026-08-31 10:05:00");
    trailing.takeProfit = 4010.0;
    trailing.stopLoss = 3997.0;
    trailing.price = 4008.0;
    trailing.reason = QStringLiteral("trailing");
    ASSERT_TRUE(SimOrderDb::insertStopUpdate(trailing));

    const auto stops = SimOrderDb::queryStopUpdates(QStringLiteral("SIM-2-0"));
    ASSERT_EQ(2, stops.size());  // open + trailing, oldest first.
    EXPECT_EQ(QStringLiteral("open"), stops[0].reason);
    EXPECT_EQ(QStringLiteral("trailing"), stops[1].reason);
    EXPECT_DOUBLE_EQ(4010.0, stops[1].takeProfit);
    EXPECT_DOUBLE_EQ(4008.0, stops[1].price);
}

TEST_F(SimOrderDbTest, CloseOrderUpdatesStatus)
{
    ASSERT_TRUE(SimOrderDb::insertOrder(makeOrder(QStringLiteral("SIM-3-0"), QStringLiteral("keyA"))));
    ASSERT_TRUE(SimOrderDb::closeOrder(QStringLiteral("SIM-3-0"), 4005.0, 5.0,
                                       QStringLiteral("2026-08-31 10:10:00")));

    const auto orders = SimOrderDb::queryOrders(QStringLiteral("keyA"));
    ASSERT_EQ(1, orders.size());
    EXPECT_EQ(QStringLiteral("closed"), orders[0].status);
    EXPECT_DOUBLE_EQ(4005.0, orders[0].exitPrice);
    EXPECT_DOUBLE_EQ(5.0, orders[0].realizedPnl);
    EXPECT_FALSE(orders[0].closedAt.isEmpty());
}

TEST_F(SimOrderDbTest, KeysAreIsolated)
{
    ASSERT_TRUE(SimOrderDb::insertOrder(makeOrder(QStringLiteral("SIM-4-0"), QStringLiteral("keyA"))));
    ASSERT_TRUE(SimOrderDb::insertOrder(makeOrder(QStringLiteral("SIM-4-1"), QStringLiteral("keyB"))));

    EXPECT_EQ(1, SimOrderDb::queryOrders(QStringLiteral("keyA")).size());
    EXPECT_EQ(1, SimOrderDb::queryOrders(QStringLiteral("keyB")).size());
    EXPECT_TRUE(SimOrderDb::queryOrders(QStringLiteral("nokey")).empty());
}

TEST_F(SimOrderDbTest, OrdersNewestFirst)
{
    auto early = makeOrder(QStringLiteral("SIM-5-0"), QStringLiteral("keyA"));
    early.openedAt = QStringLiteral("2026-08-31 09:00:00");
    auto late = makeOrder(QStringLiteral("SIM-5-1"), QStringLiteral("keyA"));
    late.openedAt = QStringLiteral("2026-08-31 11:00:00");
    ASSERT_TRUE(SimOrderDb::insertOrder(early));
    ASSERT_TRUE(SimOrderDb::insertOrder(late));

    const auto orders = SimOrderDb::queryOrders(QStringLiteral("keyA"));
    ASSERT_EQ(2, orders.size());
    EXPECT_EQ(QStringLiteral("SIM-5-1"), orders[0].orderId);  // newest first.
}
