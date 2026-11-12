#include "models/MarketFilterModel.hpp"

#include <QAbstractItemModel>

namespace pulse::client
{

MarketFilterModel::MarketFilterModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    // Any change to the visible row set re-emits count, so tab labels stay in
    // step with the list. A source reset also changes which rows pass.
    connect(this, &QAbstractItemModel::rowsInserted, this,
            &MarketFilterModel::refreshCount);
    connect(this, &QAbstractItemModel::rowsRemoved, this,
            &MarketFilterModel::refreshCount);
    connect(this, &QAbstractItemModel::modelReset, this,
            &MarketFilterModel::refreshCount);
    connect(this, &QAbstractItemModel::layoutChanged, this,
            &MarketFilterModel::refreshCount);
}

void MarketFilterModel::setMarketType(const QString& marketType)
{
    if (marketType == m_marketType)
    {
        return;
    }

    // Qt 6.13 deprecates invalidateFilter(); begin/endFilterChange uses the
    // incremental path, which emits row insert/remove signals the count
    // connections above already follow.
    beginFilterChange();
    m_marketType = marketType;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);

    emit marketTypeChanged();
    refreshCount();
}

void MarketFilterModel::setSourceModel(QAbstractItemModel* sourceModel)
{
    // Role ids are assigned per source model, so a cached id would be wrong
    // after a switch.
    m_marketRole = -1;
    QSortFilterProxyModel::setSourceModel(sourceModel);
}

bool MarketFilterModel::filterAcceptsRow(int sourceRow,
                                         const QModelIndex& sourceParent) const
{
    if (m_marketType.isEmpty())
    {
        return true;
    }

    QAbstractItemModel* source = sourceModel();
    if (nullptr == source)
    {
        return false;
    }

    // JsonListModel registers roles from the first observed row, and a proxy
    // may be queried before that payload arrives. Resolve the id lazily and
    // cache a HIT only: caching a miss would hide every row forever if the
    // first payload predates the role. Role sets only grow, so a hit stays
    // valid for the lifetime of this source.
    int role = m_marketRole;
    if (role < 0)
    {
        role = source->roleNames().key("market_type", -1);
        if (role < 0)
        {
            return false;
        }
        m_marketRole = role;
    }

    const QModelIndex index = source->index(sourceRow, 0, sourceParent);
    return source->data(index, role).toString() == m_marketType;
}

void MarketFilterModel::refreshCount()
{
    emit countChanged();
}

} // namespace pulse::client