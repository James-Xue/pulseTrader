#include "models/RiskModel.hpp"

#include <QJsonValue>

namespace pulse::client
{

RiskModel::RiskModel(QObject* parent)
    : QObject(parent)
{
}

void RiskModel::setFromJson(const QJsonObject& risk)
{
    // 1. An empty result means the engine did not report a snapshot. Keep the
    //    presence flag false so the tiles render `unknown`, never a zero.
    m_present = !risk.isEmpty();
    if (!m_present)
    {
        m_tradingHalted = false;
        m_haltReasonCode = 0;
        m_dailyDrawdown = 0.0;
        m_maxDrawdown = 0.0;
        m_rateLimiterTokens = 0.0;
        m_rateLimiterExhausted = false;
        m_portfolioPresent = false;
        m_portfolioOpenPositions = 0;
        m_portfolioNotional = 0.0;
        m_portfolioUnrealizedPnl = 0.0;
        m_portfolioNetExposure = 0.0;
        m_openPositionCount = 0;
        emit changed();
        return;
    }

    // 2. Top-level scalars. The key casing is mixed and must stay exact; a
    //    typo yields undefined, not a compile error.
    m_tradingHalted = risk.value(QStringLiteral("trading_halted")).toBool(false);
    m_haltReasonCode = risk.value(QStringLiteral("haltReason")).toInt(0);
    m_dailyDrawdown = risk.value(QStringLiteral("dailyDrawdown")).toDouble(0.0);
    m_maxDrawdown = risk.value(QStringLiteral("maxDrawdown")).toDouble(0.0);
    m_rateLimiterTokens =
        risk.value(QStringLiteral("rate_limiter_tokens")).toDouble(0.0);
    m_rateLimiterExhausted =
        risk.value(QStringLiteral("rate_limiter_exhausted")).toBool(false);
    m_openPositionCount = risk.value(QStringLiteral("openPositionCount")).toInt(0);

    // 3. Portfolio block. Presence is separate from its contents: a missing
    //    block hides the grid instead of showing four zeroes.
    const QJsonObject portfolio = risk.value(QStringLiteral("portfolio")).toObject();
    m_portfolioPresent = !portfolio.isEmpty();
    if (m_portfolioPresent)
    {
        m_portfolioOpenPositions =
            portfolio.value(QStringLiteral("openPositionCount")).toInt(0);
        m_portfolioNotional =
            portfolio.value(QStringLiteral("total_notional")).toDouble(0.0);
        m_portfolioUnrealizedPnl =
            portfolio.value(QStringLiteral("total_unrealized_pnl")).toDouble(0.0);
        m_portfolioNetExposure =
            portfolio.value(QStringLiteral("net_exposure")).toDouble(0.0);
    }
    else
    {
        m_portfolioOpenPositions = 0;
        m_portfolioNotional = 0.0;
        m_portfolioUnrealizedPnl = 0.0;
        m_portfolioNetExposure = 0.0;
    }

    emit changed();
}

} // namespace pulse::client