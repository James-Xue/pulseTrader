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

// True when `table` already has `column` (used for in-place migrations of
// databases created by older builds).
bool tableHasColumn(const QString &table, const QString &column)
{
    QSqlQuery query(connection());
    if (false == query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
    {
        return false;
    }
    while (query.next())
    {
        if (query.value(1).toString() == column)
        {
            return true;
        }
    }
    return false;
}

bool ensureColumn(const QString &table, const QString &column,
                  const QString &definition)
{
    if (tableHasColumn(table, column))
    {
        return true;
    }
    return execDdl(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                       .arg(table, column, definition));
}

} // namespace

QString SimOrderDb::defaultPath()
{
    // 0. Explicit override shared with SimAccountStore/ExternalSignalStore.
    const QString overrideDir = qEnvironmentVariable("PULSETRADER_SIM_DATA_DIR");
    if (false == overrideDir.isEmpty())
    {
        QDir().mkpath(overrideDir);
        return QDir(overrideDir).filePath(QStringLiteral("sim_orders.db"));
    }

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
        "  strategy_id   TEXT NOT NULL DEFAULT 'v10',"
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
        "  realized_pnl  REAL,"
        "  max_floating_profit REAL NOT NULL DEFAULT 0,"
        "  max_floating_loss   REAL NOT NULL DEFAULT 0"
        ")"));
    // Older databases predate the excursion columns; ALTER them in place.
    const bool migrationOk =
        ensureColumn(QStringLiteral("orders"), QStringLiteral("max_floating_profit"),
                     QStringLiteral("REAL NOT NULL DEFAULT 0"))
        && ensureColumn(QStringLiteral("orders"), QStringLiteral("max_floating_loss"),
                        QStringLiteral("REAL NOT NULL DEFAULT 0"))
        && ensureColumn(QStringLiteral("orders"), QStringLiteral("strategy_id"),
                        QStringLiteral("TEXT NOT NULL DEFAULT 'v10'"));
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

    return ordersOk && migrationOk && stopsOk && indexOk;
}

bool SimOrderDb::insertOrder(const OrderRecord &order)
{
    QSqlQuery query(connection());
    query.prepare(QStringLiteral(
        "INSERT INTO orders (order_id, api_key, side, symbol, strategy_id,"
        " volume_lots, entry_price, take_profit, stop_loss, leverage,"
        " margin_usdt, fee_usdt, status, opened_at, closed_at, exit_price,"
        " realized_pnl, max_floating_profit, max_floating_loss)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, NULL, NULL, ?, ?)"));
    query.addBindValue(order.orderId);
    query.addBindValue(order.apiKey);
    query.addBindValue(order.side);
    query.addBindValue(order.symbol);
    query.addBindValue(order.strategyId.isEmpty()
                           ? QStringLiteral("v10") : order.strategyId);
    query.addBindValue(order.volumeLots);
    query.addBindValue(order.entryPrice);
    query.addBindValue(order.takeProfit);
    query.addBindValue(order.stopLoss);
    query.addBindValue(order.leverage);
    query.addBindValue(order.marginUsdt);
    query.addBindValue(order.feeUsdt);
    query.addBindValue(order.status);
    query.addBindValue(order.openedAt);
    query.addBindValue(order.maxFloatingProfit);
    query.addBindValue(order.maxFloatingLoss);
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
                            double realizedPnl, const QString &closedAt,
                            double maxFloatingProfit, double maxFloatingLoss)
{
    QSqlQuery query(connection());
    query.prepare(QStringLiteral(
        "UPDATE orders SET status = 'closed', exit_price = ?, realized_pnl = ?,"
        " closed_at = ?, max_floating_profit = ?, max_floating_loss = ?"
        " WHERE order_id = ?"));
    query.addBindValue(exitPrice);
    query.addBindValue(realizedPnl);
    query.addBindValue(closedAt);
    query.addBindValue(maxFloatingProfit);
    query.addBindValue(maxFloatingLoss);
    query.addBindValue(orderId);
    return query.exec();
}

bool SimOrderDb::clearAll(const QString &apiKey)
{
    QSqlDatabase db = connection();
    if (false == db.transaction())
    {
        return false;
    }
    // order_stops has no api_key column — delete the rows whose parent order
    // belongs to this key, then the orders themselves.
    QSqlQuery stops(db);
    stops.prepare(QStringLiteral(
        "DELETE FROM order_stops WHERE order_id IN"
        " (SELECT order_id FROM orders WHERE api_key = ?)"));
    stops.addBindValue(apiKey);
    if (false == stops.exec())
    {
        db.rollback();
        return false;
    }
    QSqlQuery orders(db);
    orders.prepare(QStringLiteral("DELETE FROM orders WHERE api_key = ?"));
    orders.addBindValue(apiKey);
    if (false == orders.exec())
    {
        db.rollback();
        return false;
    }
    return db.commit();
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
        "SELECT order_id, side, symbol, strategy_id, volume_lots, entry_price,"
        " take_profit, stop_loss, leverage, margin_usdt, fee_usdt, status,"
        " opened_at, closed_at, exit_price, realized_pnl, max_floating_profit,"
        " max_floating_loss FROM orders WHERE api_key = ?"
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
        r.strategyId = query.value(3).toString();
        if (r.strategyId.isEmpty())
        {
            r.strategyId = QStringLiteral("v10");
        }
        r.volumeLots = query.value(4).toDouble();
        r.entryPrice = query.value(5).toDouble();
        r.takeProfit = query.value(6).toDouble();
        r.stopLoss = query.value(7).toDouble();
        r.leverage = query.value(8).toDouble();
        r.marginUsdt = query.value(9).toDouble();
        r.feeUsdt = query.value(10).toDouble();
        r.status = query.value(11).toString();
        r.openedAt = query.value(12).toString();
        r.closedAt = query.value(13).toString();
        r.exitPrice = query.value(14).toDouble();
        r.realizedPnl = query.value(15).toDouble();
        r.maxFloatingProfit = query.value(16).toDouble();
        r.maxFloatingLoss = query.value(17).toDouble();
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
