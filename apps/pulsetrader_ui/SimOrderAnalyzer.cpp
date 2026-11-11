// sim_order_analyzer.cpp — implementation of the order analysis.

#include "SimOrderAnalyzer.hpp"

#include "SimOrderDb.hpp"

#include <cmath>

namespace pulse::ui
{

namespace
{

// Tolerance for matching the exit price to a stop price (prices are
// compared as doubles; settlement uses the exact trigger value).
constexpr double kPriceTolerance = 0.005;

QString inferCloseReason(double exitPrice, double tpFinal, double slFinal,
                         const QString &side)
{
    if (exitPrice <= 0.0)
    {
        return {};
    }
    const bool isLong = (side != QStringLiteral("short"));
    // The exit price equals the triggered stop exactly (settlement fills at
    // the trigger price) — match with a small tolerance.
    if (std::abs(exitPrice - tpFinal) <= kPriceTolerance)
    {
        return QStringLiteral("TP");
    }
    if (std::abs(exitPrice - slFinal) <= kPriceTolerance)
    {
        return QStringLiteral("SL");
    }
    // Fallback: direction-aware comparison (should not happen in practice).
    return isLong ? (exitPrice > tpFinal ? QStringLiteral("TP") : QStringLiteral("SL"))
                  : (exitPrice < tpFinal ? QStringLiteral("TP") : QStringLiteral("SL"));
}

} // namespace

AnalysisReport SimOrderAnalyzer::analyze(const QString &apiKey)
{
    AnalysisReport report;
    const auto orders = SimOrderDb::queryOrders(apiKey);
    report.totalOrders = static_cast<int>(orders.size());

    for (const auto &order : orders)
    {
        OrderSummary summary;
        summary.orderId = order.orderId;
        summary.side = order.side;
        summary.openedAt = order.openedAt;
        summary.closedAt = order.closedAt;
        summary.entryPrice = order.entryPrice;
        summary.tpInitial = order.takeProfit;
        summary.slInitial = order.stopLoss;
        summary.tpFinal = order.takeProfit;
        summary.slFinal = order.stopLoss;
        summary.exitPrice = order.exitPrice;
        summary.realizedPnl = order.realizedPnl;

        // Trailing history: stops[0] is the "open" attachment; the rest are
        // trailing moves. The final stops come from the last row.
        const auto stops = SimOrderDb::queryStopUpdates(order.orderId);
        if (stops.size() > 1)
        {
            summary.stopMoves = static_cast<int>(stops.size()) - 1;
            summary.tpFinal = stops.back().takeProfit;
            summary.slFinal = stops.back().stopLoss;
        }

        const bool closed = (order.status == QStringLiteral("closed"));
        if (closed)
        {
            report.closedOrders++;
            summary.closeReason = inferCloseReason(order.exitPrice,
                                                   summary.tpFinal,
                                                   summary.slFinal,
                                                   order.side);
            if (order.realizedPnl > 0.0)
            {
                report.wins++;
            }
            else
            {
                report.losses++;
            }
            report.totalPnl += order.realizedPnl;
            if (summary.closeReason == QStringLiteral("TP"))
            {
                report.tpCloses++;
            }
            else if (summary.closeReason == QStringLiteral("SL"))
            {
                report.slCloses++;
            }
        }
        else
        {
            report.openOrders++;
        }

        if (order.side != QStringLiteral("short"))
        {
            report.longCount++;
            report.longPnl += closed ? order.realizedPnl : 0.0;
        }
        else
        {
            report.shortCount++;
            report.shortPnl += closed ? order.realizedPnl : 0.0;
        }

        report.orders.push_back(std::move(summary));
    }
    return report;
}

} // namespace pulse::ui
