// sim_order_db.cpp — SQLite order store implementation (Qt QSQLITE driver).

#include "SimOrderDb.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace pulse::ui
{

namespace
{

// One shared connection (single-threaded use — the UI thread).
constexpr const char *kConnection = "sim_orders";

bool driverAvailable()
{
    return QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"));
}

QSqlDatabase connection()
{
    return QSqlDatabase::database(QLatin1String(kConnection));
}

// Execute a DDL statement; returns false on failure.
bool execDdl(const QString &sql)
{
    QSqlQuery query(connection());
    if (false == query.exec(sql))
    {
        return false;
    }
    return true;
}

} // namespace

QString SimOrderDb::defaultPath()
{
    // 1. data/ under the process working directory (project root when
    //    launched from the repo — matches the engine's data/ convention).
    const QString cwdPath = QDir::current().filePath(QStringLiteral("data/sim_orders.db"));
    if (QDir(QDir::current().filePath(QStringLiteral("data"))).exists())
    {
        return cwdPath;
    }
    // 2. data/ under the executable's directory (double-clicked launcher).
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("data/sim_orders.db"));
}

bool SimOrderDb::init(const QString &dbPath)
{
    if (false == driverAvailable())
    {
        return false;
    }
    if (connection().isOpen())
    {
        return true;  // Already initialised.
    }

    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                QLatin1String(kConnection));
    db.setDatabaseName(dbPath);
    if (false == db.open())
    {
        return false;
    }

    // Orders: one row per opening.
    const bool ordersOk = execDdl(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS orders ("
        "  order_id      TEXT PRIMARY KEY,"
        "  api_key       TEXT NOT NULL,"
        "  side          TEXT NOT NULL,"
        "  symbol        TEXT NOT NULL,"
        "  volume_lots   REAL NOT NULL,"
        "  entry_price   REAL NOT NULL,"
        "  take_profit   REAL NOT NULL,"
        "  stop_loss     REAL NOT NULL,"
        "  leverage      REAL NOT NULL,"
        "  margin_usdt   REAL NOT NULL,"
        "  fee_usdt      REAL NOT NULL,"
        "  status        TEXT NOT NULL,"
        "  opened_at     TEXT NOT NULL,"
        "  closed_at     TEXT,"
        "  exit_price    REAL,"
        "  realized_pnl  REAL"
        ")"));
    // Stop updates: every TP/SL change on an order.
    const bool stopsOk = execDdl(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS order_stops ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  order_id     TEXT NOT NULL,"
        "  updated_at   TEXT NOT NULL,"
        "  take_profit  REAL NOT NULL,"
        "  stop_loss    REAL NOT NULL,"
        "  price        REAL NOT NULL,"
        "  reason       TEXT NOT NULL"
        ")"));
    const bool indexOk = execDdl(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_order_stops_order ON order_stops(order_id)"));

    return ordersOk && stopsOk && indexOk;
}

bool SimOrderDb::insertOrder(const OrderRecord &order)
{
    QSqlQuery query(connection());
    query.prepare(QStringLiteral(
        "INSERT INTO orders (order_id, api_key, side, symbol, volume_lots,"
        " entry_price, take_profit, stop_loss, leverage, margin_usdt, fee_usdt,"
        " status, opened_at, closed_at, exit_price, realized_pnl)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, NULL, NULL)"));
    query.addBindValue(order.orderId);
    query.addBindValue(order.apiKey);
    query.addBindValue(order.side);
    query.addBindValue(order.symbol);
    query.addBindValue(order.volumeLots);
    query.addBindValue(order.entryPrice);
    query.addBindValue(order.takeProfit);
    query.addBindValue(order.stopLoss);
    query.addBindValue(order.leverage);
    query.addBindValue(order.marginUsdt);
    query.addBindValue(order.feeUsdt);
    query.addBindValue(order.status);
    query.addBindValue(order.openedAt);
    if (false == query.exec())
    {
        return false;
    }

    // Initial stop attachment is part of the open record.
    StopUpdateRecord initial;
    initial.orderId = order.orderId;
    initial.updatedAt = order.openedAt;
    initial.takeProfit = order.takeProfit;
    initial.stopLoss = order.stopLoss;
    initial.price = order.entryPrice;
    initial.reason = QStringLiteral("open");
    return insertStopUpdate(initial);
}

bool SimOrderDb::closeOrder(const QString &orderId, double exitPrice,
                            double realizedPnl, const QString &closedAt)
{
    QSqlQuery query(connection());
    query.prepare(QStringLiteral(
        "UPDATE orders SET status = 'closed', exit_price = ?, realized_pnl = ?,"
        " closed_at = ? WHERE order_id = ?"));
    query.addBindValue(exitPrice);
    query.addBindValue(realizedPnl);
    query.addBindValue(closedAt);
    query.addBindValue(orderId);
    return query.exec();
}

bool SimOrderDb::insertStopUpdate(const StopUpdateRecord &update)
{
    QSqlQuery query(connection());
    query.prepare(QStringLiteral(
        "INSERT INTO order_stops (order_id, updated_at, take_profit, stop_loss,"
        " price, reason) VALUES (?, ?, ?, ?, ?, ?)"));
    query.addBindValue(update.orderId);
    query.addBindValue(update.updatedAt);
    query.addBindValue(update.takeProfit);
    query.addBindValue(update.stopLoss);
    query.addBindValue(update.price);
    query.addBindValue(update.reason);
    return query.exec();
}

std::vector<OrderRecord> SimOrderDb::queryOrders(const QString &apiKey)
{
    std::vector<OrderRecord> out;
    QSqlQuery query(connection());
    query.prepare(QStringLiteral(
        "SELECT order_id, side, symbol, volume_lots, entry_price, take_profit,"
        " stop_loss, leverage, margin_usdt, fee_usdt, status, opened_at,"
        " closed_at, exit_price, realized_pnl FROM orders WHERE api_key = ?"
        " ORDER BY opened_at DESC"));
    query.addBindValue(apiKey);
    if (false == query.exec())
    {
        return out;
    }
    while (query.next())
    {
        OrderRecord r;
        r.orderId = query.value(0).toString();
        r.side = query.value(1).toString();
        r.symbol = query.value(2).toString();
        r.volumeLots = query.value(3).toDouble();
        r.entryPrice = query.value(4).toDouble();
        r.takeProfit = query.value(5).toDouble();
        r.stopLoss = query.value(6).toDouble();
        r.leverage = query.value(7).toDouble();
        r.marginUsdt = query.value(8).toDouble();
        r.feeUsdt = query.value(9).toDouble();
        r.status = query.value(10).toString();
        r.openedAt = query.value(11).toString();
        r.closedAt = query.value(12).toString();
        r.exitPrice = query.value(13).toDouble();
        r.realizedPnl = query.value(14).toDouble();
        out.push_back(r);
    }
    return out;
}

std::vector<StopUpdateRecord> SimOrderDb::queryStopUpdates(const QString &orderId)
{
    std::vector<StopUpdateRecord> out;
    QSqlQuery query(connection());
    query.prepare(QStringLiteral(
        "SELECT order_id, updated_at, take_profit, stop_loss, price, reason"
        " FROM order_stops WHERE order_id = ? ORDER BY id ASC"));
    query.addBindValue(orderId);
    if (false == query.exec())
    {
        return out;
    }
    while (query.next())
    {
        StopUpdateRecord r;
        r.orderId = query.value(0).toString();
        r.updatedAt = query.value(1).toString();
        r.takeProfit = query.value(2).toDouble();
        r.stopLoss = query.value(3).toDouble();
        r.price = query.value(4).toDouble();
        r.reason = query.value(5).toString();
        out.push_back(r);
    }
    return out;
}

} // namespace pulse::ui
