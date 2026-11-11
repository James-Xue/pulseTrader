// sim_trade_engine.cpp — implementation of the paper-trading engine.

#include "SimTradeEngine.hpp"

#include <QDateTime>

#include <algorithm>

namespace pulse::ui
{

namespace
{

// Build the human-readable timestamp for the store.
QString nowStamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

} // namespace

namespace
{

// Shared open-core for both sides. `feeUsdt` is 0 for shorts (sell side is
// free in the real CFD fee model). The caller pre-validates the TP/SL
// orientation for its side.
SimTradeEngine::OpenResult openPosition(const QString &apiKey, const QString &side,
                                        double volumeLots, double entryPrice,
                                        double takeProfit, double stopLoss,
                                        double leverage, double feeUsdt,
                                        std::int64_t nowMs)
{
    SimTradeEngine::OpenResult result;
    if (volumeLots <= 0.0 || entryPrice <= 0.0)
    {
        result.error = QStringLiteral("开仓参数无效（手数 %1，价格 %2）")
                           .arg(volumeLots)
                           .arg(entryPrice);
        return result;
    }
    if (leverage < 1.0)
    {
        result.error = QStringLiteral("杠杆倍数无效：%1").arg(leverage);
        return result;
    }
    if (takeProfit <= 0.0 || stopLoss <= 0.0)
    {
        result.error = QStringLiteral("真实 CFD 开仓必须同时带止盈止损（TP/SL 缺失）");
        return result;
    }

    // Real CFD margin model.
    const double notional = volumeLots * SimTradeEngine::kOzPerLot * entryPrice;
    result.marginUsdt = notional / leverage;
    result.feeUsdt = feeUsdt;

    // Balance check — the real account rejects the order on insufficient
    // margin; the paper account behaves the same.
    const auto account = SimAccountStore::load(apiKey);
    if (false == account.has_value())
    {
        result.error = QStringLiteral("未设置模拟账户，请先点击“模拟账户”设置初始余额");
        return result;
    }
    const double required = result.marginUsdt + result.feeUsdt;
    if (account->balanceUsdt < required)
    {
        result.error = QStringLiteral("模拟账户余额不足（需保证金 %1 + 手续费 %2 = %3 USDT，"
                                      "可用 %4 USDT）")
                           .arg(result.marginUsdt, 0, 'f', 2)
                           .arg(result.feeUsdt, 0, 'f', 2)
                           .arg(required, 0, 'f', 2)
                           .arg(account->balanceUsdt, 0, 'f', 2);
        return result;
    }

    // Deduct margin + fee, append the position.
    SimAccount updated = *account;
    updated.balanceUsdt -= required;
    updated.updatedAt = nowStamp();

    // Unique order id: timestamp + in-process sequence (uniqueness holds
    // across restarts because the timestamp differs, and within a millisecond
    // because of the counter).
    static std::atomic<int> s_orderSeq{ 0 };
    SimPosition position;
    position.id = QStringLiteral("SIM-%1-%2")
                      .arg(nowMs)
                      .arg(s_orderSeq.fetch_add(1));
    position.side = side;
    position.volumeLots = volumeLots;
    position.entryPrice = entryPrice;
    position.takeProfit = takeProfit;
    position.stopLoss = stopLoss;
    position.marginUsdt = result.marginUsdt;
    position.feeUsdt = result.feeUsdt;
    position.leverage = leverage;
    position.openedAtMs = nowMs;
    updated.positions.push_back(position);

    if (false == SimAccountStore::saveAccount(apiKey, updated))
    {
        result.error = QStringLiteral("模拟账户保存失败：%1")
                           .arg(SimAccountStore::filePath());
        return result;
    }

    // Persist the order + its initial stop attachment for post-trade
    // analysis. A DB failure must not block the paper trade itself.
    OrderRecord record;
    record.orderId = position.id;
    record.apiKey = apiKey;
    record.side = side;
    record.symbol = QStringLiteral("XAUUSD");
    record.volumeLots = position.volumeLots;
    record.entryPrice = position.entryPrice;
    record.takeProfit = position.takeProfit;
    record.stopLoss = position.stopLoss;
    record.leverage = position.leverage;
    record.marginUsdt = position.marginUsdt;
    record.feeUsdt = position.feeUsdt;
    record.status = QStringLiteral("open");
    record.openedAt = nowStamp();
    SimOrderDb::insertOrder(record);

    result.ok = true;
    result.positionId = position.id;
    return result;
}

} // namespace

SimTradeEngine::OpenResult SimTradeEngine::openLong(const QString &apiKey,
                                                    double volumeLots,
                                                    double entryPrice,
                                                    double takeProfit,
                                                    double stopLoss,
                                                    double leverage,
                                                    std::int64_t nowMs)
{
    // Real CFD rule: an opening order MUST carry both stops (checked before
    // the orientation checks so the message is precise).
    if (takeProfit <= 0.0 || stopLoss <= 0.0)
    {
        OpenResult result;
        result.error = QStringLiteral("真实 CFD 开仓必须同时带止盈止损（TP/SL 缺失）");
        return result;
    }
    // Real CFD rule: TP above entry, SL below entry (long orientation).
    if (takeProfit <= entryPrice)
    {
        OpenResult result;
        result.error = QStringLiteral("止盈必须高于开仓价（多头 TP %1 ≤ 开仓 %2）")
                           .arg(takeProfit, 0, 'f', 2)
                           .arg(entryPrice, 0, 'f', 2);
        return result;
    }
    if (stopLoss >= entryPrice)
    {
        OpenResult result;
        result.error = QStringLiteral("止损必须低于开仓价（多头 SL %1 ≥ 开仓 %2）")
                           .arg(stopLoss, 0, 'f', 2)
                           .arg(entryPrice, 0, 'f', 2);
        return result;
    }
    return openPosition(apiKey, QStringLiteral("long"), volumeLots, entryPrice,
                        takeProfit, stopLoss, leverage,
                        volumeLots * SimTradeEngine::kFeeUsdtPerLot,  // BUY side commission.
                        nowMs);
}

SimTradeEngine::OpenResult SimTradeEngine::openShort(const QString &apiKey,
                                                     double volumeLots,
                                                     double entryPrice,
                                                     double takeProfit,
                                                     double stopLoss,
                                                     double leverage,
                                                     std::int64_t nowMs)
{
    // Real CFD rule: an opening order MUST carry both stops.
    if (takeProfit <= 0.0 || stopLoss <= 0.0)
    {
        OpenResult result;
        result.error = QStringLiteral("真实 CFD 开仓必须同时带止盈止损（TP/SL 缺失）");
        return result;
    }
    // Short orientation: TP below entry, SL above entry.
    if (takeProfit >= entryPrice)
    {
        OpenResult result;
        result.error = QStringLiteral("止盈必须低于开仓价（空头 TP %1 ≥ 开仓 %2）")
                           .arg(takeProfit, 0, 'f', 2)
                           .arg(entryPrice, 0, 'f', 2);
        return result;
    }
    if (stopLoss <= entryPrice)
    {
        OpenResult result;
        result.error = QStringLiteral("止损必须高于开仓价（空头 SL %1 ≤ 开仓 %2）")
                           .arg(stopLoss, 0, 'f', 2)
                           .arg(entryPrice, 0, 'f', 2);
        return result;
    }
    return openPosition(apiKey, QStringLiteral("short"), volumeLots, entryPrice,
                        takeProfit, stopLoss, leverage,
                        0.0,  // Sell side: no commission.
                        nowMs);
}

std::vector<SimTradeEngine::CloseResult> SimTradeEngine::checkAndClose(const QString &apiKey,
                                                                        double price,
                                                                        std::int64_t nowMs)
{
    std::vector<CloseResult> results;
    const auto account = SimAccountStore::load(apiKey);
    if (false == account.has_value() || account->positions.empty())
    {
        return results;
    }

    SimAccount updated = *account;
    auto &positions = updated.positions;

    // Iterate in open order; erase closes in place.
    for (auto it = positions.begin(); it != positions.end();)
    {
        double exitPrice = 0.0;
        QString reason;
        const bool isLong = (it->side != QStringLiteral("short"));
        if (isLong)
        {
            if (price >= it->takeProfit && it->takeProfit > 0.0)
            {
                exitPrice = it->takeProfit;  // TP fills at the TP price.
                reason = QStringLiteral("TP");
            }
            else if (price <= it->stopLoss && it->stopLoss > 0.0)
            {
                exitPrice = it->stopLoss;    // SL fills at the SL price.
                reason = QStringLiteral("SL");
            }
        }
        else
        {
            // Short: TP below the entry, SL above — triggered when the price
            // falls to the TP or rises to the SL.
            if (price <= it->takeProfit && it->takeProfit > 0.0)
            {
                exitPrice = it->takeProfit;
                reason = QStringLiteral("TP");
            }
            else if (price >= it->stopLoss && it->stopLoss > 0.0)
            {
                exitPrice = it->stopLoss;
                reason = QStringLiteral("SL");
            }
        }

        if (false == reason.isEmpty())
        {
            // Settlement: long PnL = (exit − entry) × lots × 100;
            // short PnL = (entry − exit) × lots × 100. Margin is released;
            // the closing side carries no commission in the paper model.
            const double pnl = isLong
                                   ? (exitPrice - it->entryPrice) * it->volumeLots * SimTradeEngine::kOzPerLot
                                   : (it->entryPrice - exitPrice) * it->volumeLots * SimTradeEngine::kOzPerLot;
            updated.balanceUsdt += it->marginUsdt + pnl;

            CloseResult close;
            close.ok = true;
            close.positionId = it->id;
            close.reason = reason;
            close.exitPrice = exitPrice;
            close.realizedPnl = pnl;
            close.releasedMargin = it->marginUsdt;
            results.push_back(close);

            // Persist the close (analysis); a failure must not break the
            // settlement.
            SimOrderDb::closeOrder(it->id, exitPrice, pnl, nowStamp());

            it = positions.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (false == results.empty())
    {
        updated.updatedAt = nowStamp();
        SimAccountStore::saveAccount(apiKey, updated);
    }
    return results;
}

SimTradeEngine::StopsUpdateResult SimTradeEngine::updateStops(const QString &apiKey,
                                                               double price,
                                                               double tpPoints,
                                                               double slPoints)
{
    StopsUpdateResult result;

    const auto account = SimAccountStore::load(apiKey);
    if (false == account.has_value() || account->positions.empty())
    {
        result.ok = true;  // Nothing to update — not an error.
        return result;
    }

    SimAccount updated = *account;
    for (auto &p : updated.positions)
    {
        // Trailing rule: stops move only in the favourable direction. The
        // distance envelope is computed per side from the live price —
        // long: TP/SL only move UP (price + tpPoints / price − slPoints);
        // short: TP/SL only move DOWN (price − tpPoints / price + slPoints).
        // This mirrors the real CFD trailing behaviour; a suggestion that
        // would widen risk is ignored.
        const bool isLong = (p.side != QStringLiteral("short"));
        double newTp = 0.0;
        double newSl = 0.0;
        if (isLong)
        {
            newTp = std::max(p.takeProfit, price + tpPoints);
            newSl = std::max(p.stopLoss, price - slPoints);
        }
        else
        {
            newTp = std::min(p.takeProfit, price - tpPoints);
            newSl = std::min(p.stopLoss, price + slPoints);
        }
        // Validity check is side-aware: a long's TP sits ABOVE the SL
        // (newTp > newSl), a short's TP sits BELOW the SL (newTp < newSl).
        // Keep the existing pair when the computed pair is degenerate.
        if ((isLong && newTp <= newSl) || (false == isLong && newTp >= newSl))
        {
            continue;
        }
        if (newTp != p.takeProfit || newSl != p.stopLoss)
        {
            p.takeProfit = newTp;
            p.stopLoss = newSl;
            ++result.updatedCount;

            // Append the stop move to the order history (analysis).
            StopUpdateRecord update;
            update.orderId = p.id;
            update.updatedAt = nowStamp();
            update.takeProfit = newTp;
            update.stopLoss = newSl;
            update.price = price;
            update.reason = QStringLiteral("trailing");
            SimOrderDb::insertStopUpdate(update);
        }
    }

    if (result.updatedCount > 0)
    {
        updated.updatedAt = nowStamp();
        if (false == SimAccountStore::saveAccount(apiKey, updated))
        {
            result.error = QStringLiteral("模拟账户保存失败：%1")
                               .arg(SimAccountStore::filePath());
            result.updatedCount = 0;
            return result;
        }
    }
    result.ok = true;
    return result;
}

std::vector<SimPosition> SimTradeEngine::positions(const QString &apiKey)
{
    const auto account = SimAccountStore::load(apiKey);
    if (account.has_value())
    {
        return account->positions;
    }
    return {};
}

} // namespace pulse::ui
