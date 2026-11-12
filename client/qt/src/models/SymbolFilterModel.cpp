#include "models/SymbolFilterModel.hpp"

#include <QAbstractItemModel>

namespace pulse::client
{

SymbolFilterModel::SymbolFilterModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    // Any change to the visible row set re-emits count, so headers stay in
    // step with the list. A source reset also changes which rows pass.
    connect(this, &QAbstractItemModel::rowsInserted, this,
            &SymbolFilterModel::refreshCount);
    connect(this, &QAbstractItemModel::rowsRemoved, this,
            &SymbolFilterModel::refreshCount);
    connect(this, &QAbstractItemModel::modelReset, this,
            &SymbolFilterModel::refreshCount);
    connect(this, &QAbstractItemModel::layoutChanged, this,
            &SymbolFilterModel::refreshCount);
}

void SymbolFilterModel::setSymbol(const QString& symbol)
{
    if (symbol == m_symbol)
    {
        return;
    }

    // Qt 6.13 deprecates invalidateFilter(); begin/endFilterChange uses the
    // incremental path, which emits row insert/remove signals the count
    // connections above already follow.
    beginFilterChange();
    m_symbol = symbol;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);

    emit symbolChanged();
    refreshCount();
}

void SymbolFilterModel::setSourceModel(QAbstractItemModel* sourceModel)
{
    // Role ids are assigned per source model, so a cached id would be wrong
    // after a switch.
    m_symbolRole = -1;
    QSortFilterProxyModel::setSourceModel(sourceModel);
}

bool SymbolFilterModel::filterAcceptsRow(int sourceRow,
                                         const QModelIndex& sourceParent) const
{
    if (m_symbol.isEmpty())
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
    int role = m_symbolRole;
    if (role < 0)
    {
        role = source->roleNames().key("symbol", -1);
        if (role < 0)
        {
            return false;
        }
        m_symbolRole = role;
    }

    // Whole-string equality, case-insensitive. Never contains()/startsWith():
    // XAGUSD, XAU_USDT and XAUT_USDT would all pass a substring test.
    const QModelIndex index = source->index(sourceRow, 0, sourceParent);
    const QString rowSymbol = source->data(index, role).toString();
    return 0 == rowSymbol.compare(m_symbol, Qt::CaseInsensitive);
}

void SymbolFilterModel::refreshCount()
{
    emit countChanged();
}

} // namespace pulse::client