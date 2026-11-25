#pragma once
// trade_history_dialog.hpp — non-modal paper-trade history table
//
// Opened from the 查看历史交易数据 button. The dialog is deliberately
// non-modal: the engine poll and the external-signal executor keep running
// while it is open. Rows come from sim_orders.db (persisted across restarts);
// open positions additionally read their live excursion extremes from
// sim_account.json so 最大浮盈/最大浮亏 are current before the close.

#include <QDialog>
#include <QString>

class QLabel;
class QTableWidget;

namespace pulse::ui
{

class TradeHistoryDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit TradeHistoryDialog(const QString &apiKey, QWidget *parent = nullptr);

    /// Re-read the order database and the open-position store, then rebuild
    /// the table. Safe to call while the dialog is visible.
    void reload();

  private:
    QString m_apiKey;
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;
};

} // namespace pulse::ui
