#include "models/AccountModel.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <QVariantMap>

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
    // 1. Futures block. Unlike spot and CFD, the futures fields are reported
    //    at the TOP LEVEL of the result, not under a "futures" key — see the
    //    get_account example in docs/qt-client-design.md §3.2. Reading a
    //    nested object here silently hid the futures tiles on every machine.
    //    `available` is the flag that says the exchange call succeeded.
    m_futuresPresent = account.value(QStringLiteral("available")).toBool(false);
    m_futuresCurrency = account.value(QStringLiteral("currency")).toString();
    m_futuresTotal = numberOr(account, QStringLiteral("total"));
    m_futuresAvailable = numberOr(account, QStringLiteral("available_balance"));
    m_futuresUnrealised = numberOr(account, QStringLiteral("unrealised_pnl"));
    m_futuresPositionMargin = numberOr(account, QStringLiteral("position_margin"));
    m_futuresOrderMargin = numberOr(account, QStringLiteral("order_margin"));

    // 2. Spot block. This is the only block a spot-only API key can populate,
    //    so the UI must be able to show it with no futures data at all.
    const QJsonObject spot = account.value(QStringLiteral("spot")).toObject();
    m_spotPresent = !spot.isEmpty();
    m_spotCurrency = spot.value(QStringLiteral("currency")).toString();
    m_spotTotal = numberOr(spot, QStringLiteral("total"));
    m_spotAvailable = numberOr(spot, QStringLiteral("available"));

    // 2b. Per-currency spot holdings. The engine filters zero balances and
    //     sorts by USDT value, so its order is kept as-is. A null value_usdt
    //     means "no ticker cached yet" and must not render as 0.00.
    m_spotBalances.clear();
    const QJsonArray balances =
        account.value(QStringLiteral("spot_balances")).toArray();
    m_spotBalances.reserve(balances.size());

    for (const QJsonValue& entry : balances)
    {
        const QJsonObject row = entry.toObject();
        const QJsonValue valueUsdt = row.value(QStringLiteral("value_usdt"));

        QVariantMap item;
        item[QStringLiteral("currency")] =
            row.value(QStringLiteral("currency")).toString();
        item[QStringLiteral("available")] =
            row.value(QStringLiteral("available")).toDouble();
        item[QStringLiteral("locked")] =
            row.value(QStringLiteral("locked")).toDouble();
        item[QStringLiteral("total")] =
            row.value(QStringLiteral("total")).toDouble();
        item[QStringLiteral("hasValue")] = valueUsdt.isDouble();
        item[QStringLiteral("valueUsdt")] = valueUsdt.toDouble(0.0);
        m_spotBalances.append(item);
    }

    // 3. CFD block. `total` is the TradFi account equity and `margin` the
    //    margin in use; a CFD-only key shows this and nothing else.
    const QJsonObject cfd = account.value(QStringLiteral("cfd")).toObject();
    m_cfdPresent = !cfd.isEmpty();
    m_cfdCurrency = cfd.value(QStringLiteral("currency")).toString(QStringLiteral("USD"));
    m_cfdTotal = numberOr(cfd, QStringLiteral("total"));
    m_cfdAvailable = numberOr(cfd, QStringLiteral("available"));
    m_cfdMargin = numberOr(cfd, QStringLiteral("margin"));
    m_cfdUnrealised = numberOr(cfd, QStringLiteral("unrealised_pnl"));

    emit changed();
}

} // namespace pulse::client
