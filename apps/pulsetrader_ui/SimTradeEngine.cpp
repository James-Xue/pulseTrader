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

SimTradeEngine::OpenResult SimTradeEngine::openLong(const QString &apiKey,
                                                    double volumeLots,
                                                    double entryPrice,
                                                    double takeProfit,
                                                    double stopLoss,
                                                    double leverage,
                                                    std::int64_t nowMs)
{
    OpenResult result;
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

    // 1. Real CFD rule: an opening order MUST carry both a take-profit and a
    //    stop-loss (the exchange attaches them to the entry order). A long
    //    needs TP above and SL below the entry — reject anything else,
    //    mirroring the real rejection.
    if (takeProfit <= 0.0 || stopLoss <= 0.0)
    {
        result.error = QStringLiteral("真实 CFD 开仓必须同时带止盈止损（TP/SL 缺失）");
        return result;
    }
    if (takeProfit <= entryPrice)
    {
        result.error = QStringLiteral("止盈必须高于开仓价（多头 TP %1 ≤ 开仓 %2）")
                           .arg(takeProfit, 0, 'f', 2)
                           .arg(entryPrice, 0, 'f', 2);
        return result;
    }
    if (stopLoss >= entryPrice)
    {
        result.error = QStringLiteral("止损必须低于开仓价（多头 SL %1 ≥ 开仓 %2）")
                           .arg(stopLoss, 0, 'f', 2)
                           .arg(entryPrice, 0, 'f', 2);
        return result;
    }

    // 2. Real CFD margin & fee model.
    const double notional = volumeLots * kOzPerLot * entryPrice;
    result.marginUsdt = notional / leverage;
    result.feeUsdt = volumeLots * kFeeUsdtPerLot;  // BUY side, once.

    // 2. Balance check — the real account rejects the order on insufficient
    //    margin; the paper account behaves the same.
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

    // 3. Deduct margin + fee, append the position.
    SimAccount updated = *account;
    updated.balanceUsdt -= required;
    updated.updatedAt = nowStamp();

    SimPosition position;
    position.id = QStringLiteral("SIM-%1").arg(nowMs);
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
    result.ok = true;
    result.positionId = position.id;
    return result;
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

        if (false == reason.isEmpty())
        {
            // Long settlement: PnL = (exit − entry) × lots × 100; margin is
            // released; the sell side carries no commission.
            const double pnl = (exitPrice - it->entryPrice) * it->volumeLots * kOzPerLot;
            updated.balanceUsdt += it->marginUsdt + pnl;

            CloseResult close;
            close.ok = true;
            close.positionId = it->id;
            close.reason = reason;
            close.exitPrice = exitPrice;
            close.realizedPnl = pnl;
            close.releasedMargin = it->marginUsdt;
            results.push_back(close);

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
                                                               double suggestedTp,
                                                               double suggestedSl)
{
    StopsUpdateResult result;
    if (suggestedTp <= suggestedSl)
    {
        result.error = QStringLiteral("止盈止损建议无效（TP %1 ≤ SL %2）")
                           .arg(suggestedTp, 0, 'f', 2)
                           .arg(suggestedSl, 0, 'f', 2);
        return result;
    }

    const auto account = SimAccountStore::load(apiKey);
    if (false == account.has_value() || account->positions.empty())
    {
        result.ok = true;  // Nothing to update — not an error.
        return result;
    }

    SimAccount updated = *account;
    for (auto &p : updated.positions)
    {
        // Long positions: stops move only UP (favourable direction).
        const double newTp = std::max(p.takeProfit, suggestedTp);
        const double newSl = std::max(p.stopLoss, suggestedSl);
        if (newTp <= newSl)
        {
            continue;  // Keep the existing (valid) pair.
        }
        if (newTp != p.takeProfit || newSl != p.stopLoss)
        {
            p.takeProfit = newTp;
            p.stopLoss = newSl;
            ++result.updatedCount;
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
