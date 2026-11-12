#pragma once

#include <QSortFilterProxyModel>
#include <QString>

namespace pulse::client
{

/// A `JsonListModel` view restricted to one exact `symbol`.
///
/// The Gold Traders page needs only the XAUUSD instances. A substring match
/// would be wrong: XAGUSD (silver), XAU_USDT (futures gold) and XAUT_USDT
/// (tokenized spot gold) all contain "XAU", so the comparison is whole-string
/// and case-insensitive.
///
/// An empty `symbol` accepts every row, mirroring MarketFilterModel's "All".
class SymbolFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT

    Q_PROPERTY(QString symbol READ symbol WRITE setSymbol NOTIFY symbolChanged)

    /// Number of rows that pass the filter. Exposed as a property so QML
    /// headers can show a live count without calling rowCount().
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit SymbolFilterModel(QObject* parent = nullptr);

    [[nodiscard]] QString symbol() const { return m_symbol; }

    /// Change the filter. An empty string shows every row.
    void setSymbol(const QString& symbol);

    [[nodiscard]] int count() const { return rowCount(); }

    /// A new source invalidates the cached role id (JsonListModel discovers
    /// roles from the payload, so they are model-specific).
    void setSourceModel(QAbstractItemModel* sourceModel) override;

signals:
    void symbolChanged();
    void countChanged();

protected:
    /// 1. An empty filter accepts every row.
    /// 2. Before the first payload there is no `symbol` role: reject
    ///    everything, so the page never flashes unrelated rows.
    /// 3. Otherwise compare the row's symbol with the requested value using
    ///    whole-string, case-insensitive equality.
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    void refreshCount();

    QString m_symbol;
    /// Role id of `symbol`, resolved lazily from the source model and reset
    /// whenever the source changes.
    mutable int m_symbolRole = -1;
};

} // namespace pulse::client