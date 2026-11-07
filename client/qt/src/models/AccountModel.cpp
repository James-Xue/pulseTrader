#include "models/AccountModel.hpp"

#include <QJsonValue>

namespace pulse::client
{

namespace
{
/// Read a numeric field, defaulting to 0.0 only when the block exists but the
/// field is missing. Callers gate rendering on the block's presence flag, so a
/// default never masquerades as a real balance.
double numberOr(const QJsonObject& object, const QString& key)
{
    return object.value(key).toDouble(0.0);
}
} // namespace

AccountModel::AccountModel(QObject* parent)
    : QObject(parent)
{
}

void AccountModel::setFromJson(const QJsonObject& account)
{
    // 1. Futures block. Every field inside it is optional in practice, but the
    //    block itself only appears when the exchange call succeeded.
    const QJsonObject futures = account.value(QStringLiteral("futures")).toObject();
    m_futuresPresent = !futures.isEmpty();
    m_futuresCurrency = futures.value(QStringLiteral("currency")).toString();
    m_futuresTotal = numberOr(futures, QStringLiteral("total"));
    m_futuresAvailable = numberOr(futures, QStringLiteral("available_balance"));
    m_futuresUnrealised = numberOr(futures, QStringLiteral("unrealised_pnl"));
    m_futuresPositionMargin = numberOr(futures, QStringLiteral("position_margin"));
    m_futuresOrderMargin = numberOr(futures, QStringLiteral("order_margin"));

    // 2. Spot block. This is the only block a spot-only API key can populate,
    //    so the UI must be able to show it with no futures data at all.
    const QJsonObject spot = account.value(QStringLiteral("spot")).toObject();
    m_spotPresent = !spot.isEmpty();
    m_spotCurrency = spot.value(QStringLiteral("currency")).toString();
    m_spotTotal = numberOr(spot, QStringLiteral("total"));
    m_spotAvailable = numberOr(spot, QStringLiteral("available"));

    // 3. CFD block.
    const QJsonObject cfd = account.value(QStringLiteral("cfd")).toObject();
    m_cfdPresent = !cfd.isEmpty();
    m_cfdAvailable = numberOr(cfd, QStringLiteral("available"));
    m_cfdUnrealised = numberOr(cfd, QStringLiteral("unrealised_pnl"));

    emit changed();
}

} // namespace pulse::client
