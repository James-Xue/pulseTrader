#include "models/StrategyParamsModel.hpp"

#include <QJsonValue>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

namespace pulse::client
{

const QStringList& StrategyParamsModel::canonicalKeys()
{
    // Mirrors strategy::paramGetters() in the engine. The order is the UI's
    // reading order (sizing → confidence → indicators → execution), not the
    // engine's map order.
    static const QStringList keys = {
        QStringLiteral("order_quantity"),
        QStringLiteral("min_confidence"),
        QStringLiteral("ema_fast_period"),
        QStringLiteral("ema_slow_period"),
        QStringLiteral("bb_period"),
        QStringLiteral("bb_std_dev"),
        QStringLiteral("ob_imbalance_threshold"),
        QStringLiteral("ob_depth"),
        QStringLiteral("supertrend_period"),
        QStringLiteral("supertrend_multiplier"),
        QStringLiteral("cooldown_seconds"),
        QStringLiteral("stop_loss_pct"),
        QStringLiteral("take_profit_pct"),
        QStringLiteral("auto_trade"),
    };
    return keys;
}

StrategyParamsModel::StrategyParamsModel(QObject* parent)
    : QObject(parent)
{
}

void StrategyParamsModel::setPending(const QString& strategyId)
{
    m_strategyId = strategyId;
    m_present = false;
    m_pending = true;
    m_error.clear();
    m_params.clear();
    emit changed();
}

void StrategyParamsModel::setFromJson(const QString& strategyId,
                                      const QJsonObject& params)
{
    // A reply for a previous selection is stale: the user already moved on.
    if (strategyId != m_strategyId)
    {
        return;
    }

    // 1. Known keys first, in the canonical reading order.
    QVariantList ordered;
    QSet<QString> seen;
    for (const QString& key : canonicalKeys())
    {
        if (!params.contains(key))
        {
            continue;
        }
        QVariantMap row;
        row[QStringLiteral("key")] = key;
        row[QStringLiteral("value")] = params.value(key).toVariant();
        ordered.append(row);
        seen.insert(key);
    }

    // 2. Unknown keys last, so a new engine parameter is never silently
    //    hidden. QJsonObject iterates in key order, which keeps this stable.
    for (auto it = params.begin(); it != params.end(); ++it)
    {
        if (seen.contains(it.key()) || !it.value().isDouble())
        {
            continue;
        }
        QVariantMap row;
        row[QStringLiteral("key")] = it.key();
        row[QStringLiteral("value")] = it.value().toVariant();
        ordered.append(row);
    }

    m_params = ordered;
    m_pending = false;
    m_present = true;
    m_error.clear();
    emit changed();
}

void StrategyParamsModel::setError(const QString& strategyId,
                                   const QString& message)
{
    if (strategyId != m_strategyId)
    {
        return;
    }
    m_pending = false;
    m_present = false;
    m_error = message;
    m_params.clear();
    emit changed();
}

void StrategyParamsModel::clear()
{
    if (m_strategyId.isEmpty() && m_params.isEmpty() && !m_pending
        && m_error.isEmpty())
    {
        return;
    }
    m_strategyId.clear();
    m_present = false;
    m_pending = false;
    m_error.clear();
    m_params.clear();
    emit changed();
}

} // namespace pulse::client