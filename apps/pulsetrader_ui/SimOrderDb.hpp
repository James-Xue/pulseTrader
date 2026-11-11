#pragma once
// sim_order_db.hpp — SQLite persistence for simulated CFD orders
//
// Every paper trade gets a unique order id (SIM-<ms>-<seq>) and is recorded
// together with its stop-loss / take-profit update history, so post-trade
// analysis can replay the full lifecycle:
//
//   orders        — one row per opening: order_id, key, side, symbol,
//                   volume, entry, initial TP/SL, leverage, margin, fee,
//                   status (open/closed), open/close time, exit, realized PnL
//   order_stops   — every TP/SL change: order_id, time, TP, SL, price at the
//                   update, reason ("open" for the entry attachment,
//                   "trailing" for follow-up moves)
//
// Storage: data/sim_orders.db (the data/ dir is gitignored). Uses the Qt
// built-in QSQLITE driver — no extra dependency, windeployqt ships the
// sqlite plugin.
//
// Thread safety: all writes happen on the UI thread (open/close/trailing all
// run in the launcher's UI callbacks), so a single connection suffices.

#include <QString>

#include <vector>

namespace pulse::ui
{

/// One order row (orders table).
struct OrderRecord
{
    QString orderId;
    QString apiKey;
    QString side;            ///< "long" / "short".
    QString symbol;          ///< "XAUUSD".
    double volumeLots = 0.0;
    double entryPrice = 0.0;
    double takeProfit = 0.0; ///< TP at open.
    double stopLoss = 0.0;   ///< SL at open.
    double leverage = 1.0;
    double marginUsdt = 0.0;
    double feeUsdt = 0.0;
    QString status;          ///< "open" / "closed".
    QString openedAt;        ///< "yyyy-MM-dd HH:mm:ss".
    QString closedAt;
    double exitPrice = 0.0;
    double realizedPnl = 0.0;
};

/// One stop-update row (order_stops table).
struct StopUpdateRecord
{
    QString orderId;
    QString updatedAt;
    double takeProfit = 0.0;
    double stopLoss = 0.0;
    double price = 0.0;      ///< Market price at the update.
    QString reason;          ///< "open" / "trailing".
};

class SimOrderDb
{
  public:
    /// Open the database and create the schema (idempotent). Returns false
    /// when the SQLite driver is unavailable or the file cannot be opened.
    static bool init(const QString &dbPath = defaultPath());

    /// data/sim_orders.db under the working dir (project root), falling
    /// back to the executable's directory.
    static QString defaultPath();

    // -- Writes -------------------------------------------------------------

    /// Insert one opening order + its initial stop attachment ("open").
    static bool insertOrder(const OrderRecord &order);

    /// Mark an order closed with its exit price and realized PnL.
    static bool closeOrder(const QString &orderId, double exitPrice,
                           double realizedPnl, const QString &closedAt);

    /// Append one TP/SL update (reason "trailing").
    static bool insertStopUpdate(const StopUpdateRecord &update);

    // -- Reads (post-trade analysis) ----------------------------------------

    /// All orders for a key, newest first.
    static std::vector<OrderRecord> queryOrders(const QString &apiKey);

    /// Full stop-update history for one order, oldest first.
    static std::vector<StopUpdateRecord> queryStopUpdates(const QString &orderId);
};

} // namespace pulse::ui
