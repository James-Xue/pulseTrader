#include "models/JsonListModel.hpp"

#include <QJsonValue>

namespace pulse::client
{

JsonListModel::JsonListModel(QString keyField, QObject* parent)
    : QAbstractListModel(parent)
    , m_keyField(std::move(keyField))
{
}

void JsonListModel::registerRoles(const QJsonObject& sample)
{
    // 1. First observation decides the role set. The engine's key sets are
    //    stable per method, so this happens once per model in practice.
    // 2. Existing ids are preserved, which keeps QML bindings alive when a
    //    later payload adds a key.
    for (auto it = sample.begin(); it != sample.end(); ++it)
    {
        if (m_roleIds.contains(it.key()))
        {
            continue;
        }
        const int id = m_nextRoleId++;
        m_roleIds.insert(it.key(), id);
        m_roles.insert(id, it.key().toUtf8());
    }
}

QString JsonListModel::keyOf(const QJsonObject& row) const
{
    // The key field is always a string or a number in these payloads. Numbers
    // are formatted without decimals so `id`-style fields stay stable.
    const QJsonValue value = row.value(m_keyField);
    if (value.isString())
    {
        return value.toString();
    }
    if (value.isDouble())
    {
        return QString::number(static_cast<qint64>(value.toDouble()));
    }
    return QString();
}

void JsonListModel::setRows(const QJsonArray& rows)
{
    if (!rows.isEmpty())
    {
        registerRoles(rows.first().toObject());
    }

    // 1. Compare the key sequence. Same keys in the same order means an
    //    in-place update; anything else resets the model, because QML
    //    selection and scroll position must not survive a reorder.
    bool sameSequence = (rows.size() == m_rows.size());
    if (sameSequence)
    {
        for (int i = 0; i < rows.size(); ++i)
        {
            if (keyOf(rows.at(i).toObject()) != keyOf(m_rows.at(i)))
            {
                sameSequence = false;
                break;
            }
        }
    }

    if (!sameSequence)
    {
        beginResetModel();
        m_rows.clear();
        for (const QJsonValue& value : rows)
        {
            m_rows.append(value.toObject());
        }
        endResetModel();
        emit countChanged();
        return;
    }

    if (m_rows.isEmpty())
    {
        return;
    }

    for (int i = 0; i < rows.size(); ++i)
    {
        m_rows[i] = rows.at(i).toObject();
    }
    emit dataChanged(index(0), index(m_rows.size() - 1));
    emit countChanged();
}

void JsonListModel::clearRows()
{
    if (m_rows.isEmpty())
    {
        return;
    }
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit countChanged();
}

int JsonListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant JsonListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
    {
        return {};
    }
    const QByteArray name = m_roles.value(role);
    if (name.isEmpty())
    {
        return {};
    }
    return m_rows.at(index.row()).value(QString::fromUtf8(name)).toVariant();
}

QHash<int, QByteArray> JsonListModel::roleNames() const
{
    return m_roles;
}

} // namespace pulse::client
