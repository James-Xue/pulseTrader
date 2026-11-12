#pragma once

#include <QSortFilterProxyModel>
#include <QString>

namespace pulse::client
{

/// A `JsonListModel` view restricted to one `market_type`.
///
/// The engine stamps `market_type` ("spot" | "futures" | "cfd") on every
/// position, active order and recent fill. The Qt client keeps one shared
/// source model per kind and exposes three filtered views, so the dashboard
/// tabs, the positions page and the orders page all read the same snapshot
/// through different lenses — no second poll, no second copy of the data.
///
/// An empty `marketType` accepts every row ("All" segments).
class MarketFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT

    Q_PROPERTY(QString marketType READ marketType WRITE setMarketType
                   NOTIFY marketTypeChanged)

    /// Number of rows that pass the filter. Exposed as a property so QML tab
    /// labels can show live counts ("Spot (2)") without reaching into the
    /// model's rowCount() from a binding.
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit MarketFilterModel(QObject* parent = nullptr);

    [[nodiscard]] QString marketType() const { return m_marketType; }

    /// Change the filter. An empty string shows every row.
    void setMarketType(const QString& marketType);

    [[nodiscard]] int count() const { return rowCount(); }

    /// A new source invalidates the cached role id (roles are discovered from
    /// the payload by JsonListModel, so they are model-specific).
    void setSourceModel(QAbstractItemModel* sourceModel) override;

signals:
    void marketTypeChanged();
    void countChanged();

protected:
    /// 1. "All" accepts every row.
    /// 2. Before the first payload there is no `market_type` role: reject
    ///    everything, so a market tab never flashes another market's rows.
    /// 3. Otherwise compare the row's market_type with the requested value.
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    void refreshCount();

    QString m_marketType;
    /// Role id of `market_type`, resolved lazily from the source model and
    /// reset whenever the source changes.
    mutable int m_marketRole = -1;
};

} // namespace pulse::client