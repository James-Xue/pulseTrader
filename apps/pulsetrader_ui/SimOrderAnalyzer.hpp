#pragma once
// sim_order_analyzer.hpp — post-trade analysis over the SQLite order store
//
// Replays every order's lifecycle from the orders + order_stops tables:
//   - per-order: open info, the initial TP/SL, every trailing move (count +
//     final stops), close reason (TP/SL inferred from the exit price), PnL
//   - aggregates: win rate, total PnL, TP-vs-SL closes, long/short split
//
// Pure logic over SimOrderDb — unit-testable without a UI.

#include <QString>

#include <vector>

namespace pulse::ui
{

/// One order's lifecycle summary (analysis view).
struct OrderSummary
{
    QString orderId;
    QString side;              ///< "long" / "short".
    QString openedAt;
    QString closedAt;
    double entryPrice = 0.0;
    double tpInitial = 0.0;
    double slInitial = 0.0;
    double tpFinal = 0.0;      ///< After the last trailing move (== initial if none).
    double slFinal = 0.0;
    int stopMoves = 0;         ///< Number of trailing updates after the open.
    QString closeReason;       ///< "TP" / "SL" / "" (still open).
    double exitPrice = 0.0;
    double realizedPnl = 0.0;
};

/// Aggregated report for one API key.
struct AnalysisReport
{
    int totalOrders = 0;
    int closedOrders = 0;
    int openOrders = 0;
    int wins = 0;              ///< Closed with realizedPnl > 0.
    int losses = 0;            ///< Closed with realizedPnl <= 0.
    double totalPnl = 0.0;
    int tpCloses = 0;
    int slCloses = 0;
    int longCount = 0;
    double longPnl = 0.0;
    int shortCount = 0;
    double shortPnl = 0.0;
    std::vector<OrderSummary> orders;  ///< Newest first (DB order).
};

class SimOrderAnalyzer
{
  public:
    /// Build the full analysis for `apiKey`. Empty report when the DB is
    /// unavailable or the key has no orders.
    static AnalysisReport analyze(const QString &apiKey);
};

} // namespace pulse::ui
