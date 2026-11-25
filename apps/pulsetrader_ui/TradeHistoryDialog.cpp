// trade_history_dialog.cpp — non-modal trade history table implementation.

#include "TradeHistoryDialog.hpp"

#include "SimAccountStore.hpp"
#include "SimOrderDb.hpp"

#include <QAbstractItemView>
#include <QColor>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cmath>

namespace pulse::ui
{

namespace
{

// "+1.23 USD" / "-0.45 USD" / "0.00 USD". Values that round to zero lose the
// sign so a flat result is not shown as "-0.00".
QString signedUsd(double value)
{
    if (std::abs(value) < 0.005)
    {
        return QStringLiteral("0.00 USD");
    }
    const QString sign = (value > 0.0) ? QStringLiteral("+") : QString();
    return sign + QString::number(value, 'f', 2) + QStringLiteral(" USD");
}

QTableWidgetItem *makeCell(const QString &text, Qt::Alignment alignment)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

} // namespace

TradeHistoryDialog::TradeHistoryDialog(const QString &apiKey, QWidget *parent)
    : QDialog(parent)
    , m_apiKey(apiKey)
{
    setWindowTitle(QStringLiteral("历史交易数据"));
    setModal(false);
    resize(1120, 560);

    auto *layout = new QVBoxLayout(this);

    m_summary = new QLabel(QStringLiteral("加载中..."), this);
    layout->addWidget(m_summary);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(10);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("开仓价格"),
        QStringLiteral("开仓时间"),
        QStringLiteral("开仓状态"),
        QStringLiteral("策略"),
        QStringLiteral("开仓手"),
        QStringLiteral("平仓价格"),
        QStringLiteral("平仓时间"),
        QStringLiteral("平仓净盈亏"),
        QStringLiteral("最大浮盈"),
        QStringLiteral("最大浮亏"),
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);

    QHeaderView *header = m_table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(70);
    const int widths[] = { 100, 155, 90, 120, 80, 100, 155, 110, 110, 110 };
    for (int column = 0; column < m_table->columnCount()
         && column < static_cast<int>(sizeof(widths) / sizeof(widths[0])); ++column)
    {
        header->setSectionResizeMode(column, QHeaderView::Interactive);
        header->resizeSection(column, widths[column]);
    }
    layout->addWidget(m_table, 1);

    auto *buttons = new QHBoxLayout();
    auto *refresh = new QPushButton(QStringLiteral("刷新"), this);
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    connect(refresh, &QPushButton::clicked, this, &TradeHistoryDialog::reload);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addStretch();
    buttons->addWidget(refresh);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    reload();
}

void TradeHistoryDialog::reload()
{
    if (nullptr == m_table)
    {
        return;
    }

    const auto orders = SimOrderDb::queryOrders(m_apiKey);

    // Open positions carry the live excursion extremes in the account store;
    // sim_orders.db only receives them when the order closes.
    QHash<QString, SimPosition> openPositions;
    if (const auto account = SimAccountStore::load(m_apiKey); account.has_value())
    {
        for (const auto &position : account->positions)
        {
            openPositions.insert(position.id, position);
        }
    }

    int openCount = 0;
    int closedCount = 0;
    int wins = 0;
    int losses = 0;
    double totalPnl = 0.0;
    m_table->setRowCount(static_cast<int>(orders.size()));

    for (int row = 0; row < static_cast<int>(orders.size()); ++row)
    {
        const OrderRecord &order = orders[static_cast<std::size_t>(row)];
        const bool isOpen = (order.status != QStringLiteral("closed"));
        // Opening commission is charged separately when the position opens;
        // sim_orders.db stores the gross settlement PnL. Show the net trade
        // result so the history total reconciles with the account balance.
        const double netPnl = order.realizedPnl - order.feeUsdt;
        if (isOpen)
        {
            ++openCount;
        }
        else
        {
            ++closedCount;
            totalPnl += netPnl;
            if (netPnl > 0.0)
            {
                ++wins;
            }
            else if (netPnl < 0.0)
            {
                ++losses;
            }
        }

        double maxProfit = order.maxFloatingProfit;
        double maxLoss = order.maxFloatingLoss;
        if (isOpen)
        {
            const auto position = openPositions.constFind(order.orderId);
            if (position != openPositions.constEnd())
            {
                maxProfit = position->maxFloatingProfitUsd;
                maxLoss = position->maxFloatingLossUsd;
            }
        }

        const QString side = (order.side == QStringLiteral("short"))
                                 ? QStringLiteral("空仓")
                                 : QStringLiteral("多仓");
        QTableWidgetItem *entryItem = makeCell(
            QString::number(order.entryPrice, 'f', 2),
            Qt::AlignRight | Qt::AlignVCenter);
        entryItem->setToolTip(
            QStringLiteral("订单号：%1\n品种：%2\n杠杆：%3x\n手续费：%4 USD")
                .arg(order.orderId, order.symbol)
                .arg(order.leverage, 0, 'f', 0)
                .arg(order.feeUsdt, 0, 'f', 2));
        m_table->setItem(row, 0, entryItem);

        m_table->setItem(row, 1, makeCell(order.openedAt,
                                          Qt::AlignLeft | Qt::AlignVCenter));
        m_table->setItem(row, 2, makeCell(side, Qt::AlignCenter));
        m_table->setItem(row, 3, makeCell(
            order.strategyId.isEmpty() ? QStringLiteral("v10") : order.strategyId,
            Qt::AlignCenter));
        m_table->setItem(row, 4, makeCell(QString::number(order.volumeLots, 'f', 2),
                                          Qt::AlignRight | Qt::AlignVCenter));

        if (isOpen)
        {
            m_table->setItem(row, 5, makeCell(QStringLiteral("—"), Qt::AlignRight
                                                                      | Qt::AlignVCenter));
            m_table->setItem(row, 6, makeCell(QStringLiteral("—"), Qt::AlignLeft
                                                                      | Qt::AlignVCenter));
            m_table->setItem(row, 7, makeCell(QStringLiteral("持仓中"),
                                              Qt::AlignRight | Qt::AlignVCenter));
        }
        else
        {
            m_table->setItem(row, 5, makeCell(
                QString::number(order.exitPrice, 'f', 2),
                Qt::AlignRight | Qt::AlignVCenter));
            m_table->setItem(row, 6, makeCell(order.closedAt,
                                              Qt::AlignLeft | Qt::AlignVCenter));
            QTableWidgetItem *pnlItem = makeCell(
                signedUsd(netPnl),
                Qt::AlignRight | Qt::AlignVCenter);
            pnlItem->setToolTip(
                QStringLiteral("毛盈亏：%1\n手续费：%2\n净盈亏：%3")
                    .arg(signedUsd(order.realizedPnl))
                    .arg(signedUsd(order.feeUsdt))
                    .arg(signedUsd(netPnl)));
            if (netPnl > 0.0)
            {
                pnlItem->setForeground(QColor(0xC0, 0x39, 0x2B));
            }
            else if (netPnl < 0.0)
            {
                pnlItem->setForeground(QColor(0x1E, 0x8E, 0x3E));
            }
            m_table->setItem(row, 7, pnlItem);
        }

        QTableWidgetItem *maxProfitItem =
            makeCell(signedUsd(maxProfit), Qt::AlignRight | Qt::AlignVCenter);
        if (maxProfit > 0.0)
        {
            maxProfitItem->setForeground(QColor(0xC0, 0x39, 0x2B));
        }
        m_table->setItem(row, 8, maxProfitItem);

        QTableWidgetItem *maxLossItem =
            makeCell(signedUsd(maxLoss), Qt::AlignRight | Qt::AlignVCenter);
        if (maxLoss < 0.0)
        {
            maxLossItem->setForeground(QColor(0x1E, 0x8E, 0x3E));
        }
        m_table->setItem(row, 9, maxLossItem);
    }

    if (orders.empty())
    {
        m_summary->setText(QStringLiteral("暂无交易记录（开仓后会自动记录，平仓后写入盈亏与最大浮盈/浮亏）"));
    }
    else
    {
        // The old 订单分析 button printed these aggregates as text; keep the
        // useful part in the single history window instead of a second button.
        const double winRate =
            closedCount > 0 ? 100.0 * static_cast<double>(wins) / closedCount : 0.0;
        m_summary->setText(QStringLiteral(
            "共 %1 笔：持仓中 %2 笔，已平仓 %3 笔；已平仓胜率 %4%（%5 胜 / %6 负），"
            "累计净盈亏 %7（已扣开仓手续费；不含持仓保证金与浮盈，最新在上）")
                               .arg(orders.size())
                               .arg(openCount)
                               .arg(closedCount)
                               .arg(winRate, 0, 'f', 1)
                               .arg(wins)
                               .arg(losses)
                               .arg(signedUsd(totalPnl)));
    }
}

} // namespace pulse::ui
