#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace pulse::client
{

/// List model over a JSON array, with roles discovered from the data.
///
/// The engine's payloads evolve (keys are added, never renamed), so hard-coding
/// a role enum per method would mean a client release per engine key. Instead
/// every key of the first observed row becomes a role, and later rows are
/// looked up by name. QML delegates read `model.<key>` directly.
///
/// Update strategy: keyed and order-preserving. If the key sequence is
/// unchanged the rows are updated in place (selection survives); otherwise the
/// model resets.
class JsonListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    /// keyField: the field that identifies a row (`position_id`, `order_id`, ...).
    explicit JsonListModel(QString keyField, QObject* parent = nullptr);

    /// Replace the contents with a new array from the engine.
    void setRows(const QJsonArray& rows);

    /// Drop all rows (called when the connection is lost).
    void clearRows();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    void registerRoles(const QJsonObject& sample);
    [[nodiscard]] QString keyOf(const QJsonObject& row) const;

    QString m_keyField;
    QList<QJsonObject> m_rows;
    QHash<int, QByteArray> m_roles;  ///< role id -> key name
    QHash<QString, int> m_roleIds;   ///< key name -> role id
    int m_nextRoleId = Qt::UserRole + 1;
};

} // namespace pulse::client
