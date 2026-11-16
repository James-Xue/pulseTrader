// main_window.cpp — the constructor only runs ui->setupUi(this) plus the
// balance texts into label_balance / label_simBalance. The 模拟账户 flow and
// the engine polling (启动引擎 = three 1-second poll threads: XAUUSD → the
// strategy decision, and XAUUSD/USIDX/XBRUSD → their own price tape panes)
// are the active logic; all widgets come from MainWindow.ui.

#include "MainWindow.hpp"

#include "EngineStrategyAdapter.hpp"
#include "SimAccountStore.hpp"
#include "SimOrderAnalyzer.hpp"
#include "SimOrderDb.hpp"
#include "SimTradeEngine.hpp"

#include "exchange/GateRestClient.hpp"

#include "ui_MainWindow.h"

#include <QColor>
#include <QDateTime>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>

using pulse::ui::SimAccountStore;
using pulse::ui::SimTradeEngine;

namespace
{

// Poll interval: 1 s. REST ticker polling faster than that risks rate
// limiting, and gold prices move on a minutes scale anyway.
constexpr auto kPollInterval = std::chrono::seconds(1);
// How often a persistent failure is reported to the log pane.
constexpr auto kErrorReportInterval = std::chrono::seconds(10);

// Trailing column of the 真实持仓 table — one 平仓 button per row.
constexpr int kRealPositionCloseColumn = 8;
// The two editable price columns: double-clicking a cell and committing a
// value writes it back to the real Gate position.
constexpr int kTakeProfitColumn = 5;
constexpr int kStopLossColumn = 6;

// Current Beijing time as "YYYY-MM-DD HH:MM:SS", stamped onto every price
// the panes print.
//
// Derived from UTC + 8 h rather than QDateTime::currentDateTime(): China
// Standard Time is a fixed offset with no DST, so this stays correct even if
// the machine is set to another timezone or its tz database is stale — the
// log must not silently drift from the exchange's own clock.
QString beijingNow()
{
    return QDateTime::currentDateTimeUtc()
        .addSecs(8 * 3600)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// Read a numeric JSON field safely: Gate encodes monetary amounts and ids as
// STRINGS — json::value(key, 0.0) would throw type_error.302 on them (which
// inside a detached worker thread means std::terminate → crash).
double safeField(const nlohmann::json &obj, const char *key)
{
    if (obj.contains(key))
    {
        return pulse::ui::VerifyWorker::jsonToDouble(obj[key]);
    }
    return 0.0;
}

// Format a CFD quote for the 真实持仓 table. The position payload carries no
// price precision and the roster mixes 2-decimal metals/indices with 3-5
// decimal forex pairs, so print up to 5 decimals and trim the noise, keeping
// at least the two decimals the rest of the window uses.
QString formatPositionPrice(double price)
{
    if (0.0 >= price)
    {
        return QStringLiteral("-");
    }
    QString text = QString::number(price, 'f', 5);
    int decimals = 5;
    while (2 < decimals && text.endsWith(QLatin1Char('0')))
    {
        text.chop(1);
        --decimals;
    }
    return text;
}

// "+1.23" / "-0.45" — the sign is part of the value so a profit is never
// mistaken for a loss at a glance. Values that round to 0.00 are shown as
// "+0.00" rather than the misleading "-0.00".
QString formatSignedUsd(double amount)
{
    if (0.0 > amount && -0.005 < amount)
    {
        amount = 0.0;
    }
    const QString sign = (0.0 <= amount) ? QStringLiteral("+") : QString();
    return sign + QString::number(amount, 'f', 2);
}

// Payload for PUT /tradfi/positions/{id}: Gate takes the protective prices as
// decimal strings, and "0" clears one of them.
QString stopPricePayload(double price)
{
    if (0.0 >= price)
    {
        return QStringLiteral("0");
    }
    return QString::number(price, 'f', 6);
}

// XAUUSD quotes with two decimals (symbol detail: price_precision = 2).
// Rounding a computed stop here keeps the manager's comparison against the
// exchange snapshot stable: an unrounded 8U distance would be resubmitted
// forever because Gate stores the rounded value the next refresh reads back.
double roundXauPrice(double price)
{
    return std::round(price * 100.0) / 100.0;
}

// Pearson correlation of two aligned price-change series. Returns nullopt
// when either series never moved (zero variance), where r is undefined —
// e.g. a slow index that printed the same quote every minute in the window.
std::optional<double> pearsonCorrelation(const std::deque<double> &x,
                                         const std::deque<double> &y)
{
    const std::size_t n = std::min(x.size(), y.size());
    if (n < 2)
    {
        return std::nullopt;
    }
    double meanX = 0.0;
    double meanY = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        meanX += x[i];
        meanY += y[i];
    }
    meanX /= static_cast<double>(n);
    meanY /= static_cast<double>(n);

    double covariance = 0.0;
    double varianceX = 0.0;
    double varianceY = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double dx = x[i] - meanX;
        const double dy = y[i] - meanY;
        covariance += dx * dy;
        varianceX += dx * dx;
        varianceY += dy * dy;
    }
    // A flat series carries no directional information; r is 0/0.
    constexpr double kMinVariance = 1e-18;
    if (varianceX <= kMinVariance || varianceY <= kMinVariance)
    {
        return std::nullopt;
    }
    return covariance / std::sqrt(varianceX * varianceY);
}

} // namespace

// ---------------------------------------------------------------------------
// 黄金交易员 strategy roster
// ---------------------------------------------------------------------------

const std::vector<SimGateSystem::GoldTraderStrategyInfo> &
SimGateSystem::goldTraderStrategies()
{
    // Numbered from 1 in display order. To add a strategy, append its entry
    // here and add the matching dispatch in autoManageRealStops(); the
    // strategy-number combo box and the startup log follow automatically.
    static const std::vector<GoldTraderStrategyInfo> strategies = {
        GoldTraderStrategyInfo{
            1,
            QStringLiteral("动态止损"),
            QStringLiteral("亏损 → 8U 止损；盈利 > 2U → 保本损；再与 1 分钟超级趋势"
                           "（多留 1U 缓冲，避免插针）比较取更优；每秒记录峰值浮盈，"
                           "最大浮盈 − 峰值回撤（默认 5U）参与比较，回撤达到该值"
                           "立即市价平仓；只收紧不放宽。")},
    };
    return strategies;
}

int SimGateSystem::selectedGoldTraderStrategy() const
{
    const int number = ui->combo_goldTraderStrategy->currentData().toInt();
    return (0 < number) ? number : 0;
}

void SimGateSystem::logGoldTraderRoster()
{
    const auto &strategies = goldTraderStrategies();
    appendLog(QStringLiteral("[黄金交易员] 已加载策略清单（共 %1 个，下拉框可切换）：")
                  .arg(strategies.size()));
    for (const GoldTraderStrategyInfo &strategy : strategies)
    {
        appendLog(QStringLiteral("[黄金交易员]   策略 %1：%2 — %3")
                      .arg(strategy.number)
                      .arg(strategy.name, strategy.summary));
    }
    const int selected = selectedGoldTraderStrategy();
    QString selectedName;
    for (const GoldTraderStrategyInfo &strategy : strategies)
    {
        if (strategy.number == selected)
        {
            selectedName = strategy.name;
            break;
        }
    }
    appendLog(QStringLiteral("[黄金交易员] 当前选择：策略 %1（%2）"
                             "（点击“启动黄金交易员”后生效）")
                  .arg(selected)
                  .arg(selectedName.isEmpty() ? QStringLiteral("未选择")
                                              : selectedName));
}

SimGateSystem::SimGateSystem(const pulse::ui::VerifyResult &login,
                             const QString &apiKey,
                             const QString &apiSecret,
                             QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SimGateSystem)
    , m_apiKey(apiKey)
    , m_apiSecret(apiSecret)
    , m_strategyParams(std::make_shared<pulse::ui::GoldStrategyEngine::Params>())
{
    ui->setupUi(this);

    // 真实持仓 table: the symbol column takes the spare width, the numeric
    // columns size to their headers. Row content is filled by
    // onRealPositionsUpdated() from the background position refresh.
    QHeaderView *positionsHeader = ui->table_realPositions->horizontalHeader();
    positionsHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    positionsHeader->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    positionsHeader->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    positionsHeader->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    positionsHeader->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    positionsHeader->setSectionResizeMode(kTakeProfitColumn,
                                          QHeaderView::ResizeToContents);
    positionsHeader->setSectionResizeMode(kStopLossColumn,
                                          QHeaderView::ResizeToContents);
    positionsHeader->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    positionsHeader->setSectionResizeMode(kRealPositionCloseColumn,
                                          QHeaderView::ResizeToContents);
    // A little taller than the text-only rows so the 平仓 button fits.
    ui->table_realPositions->verticalHeader()->setDefaultSectionSize(28);
    // Only the TP/SL cells keep Qt::ItemIsEditable (set per item below), and
    // a committed edit is synced to the real position.
    ui->table_realPositions->setEditTriggers(QAbstractItemView::DoubleClicked
                                             | QAbstractItemView::EditKeyPressed);
    connect(ui->table_realPositions, &QTableWidget::itemChanged, this,
            &SimGateSystem::onRealPositionItemChanged);

    // Cap the trade pane so a long run cannot grow memory without bound
    // (QTextEdit has no block cap of its own — the document has). The log and
    // price tape panes are QPlainTextEdit, whose own maximumBlockCount
    // property is set in MainWindow.ui — one source of truth per pane, so
    // nothing here silently overrides the Designer value.
    ui->textEdit_trade->document()->setMaximumBlockCount(500);

    // Balance snapshot from the login verification, e.g.
    // "现货 USDT：0.006598  |  CFD：203.49 USD". The CFD part is later
    // replaced by the live account once the poll loop refreshes it.
    m_spotBalanceText = QStringLiteral("现货 USDT：%1")
                            .arg(QString::number(login.spotUsdt, 'f', 6));
    QString balance = m_spotBalanceText;
    if (login.cfdAvailable)
    {
        balance += QStringLiteral("  |  CFD：%1 USD")
                       .arg(QString::number(login.cfdEquity, 'f', 2));
    }
    else
    {
        balance += QStringLiteral("  |  CFD：不可用");
    }
    ui->label_balance->setText(balance);

    // The login verification carried the per-product permissions; extract
    // the CFD (tradfi) write flag. Read-only keys cannot modify real CFD
    // stops — the paper account mirrors that restriction.
    for (const auto &perm : login.permissions)
    {
        if ("tradfi" == perm.name && perm.known)
        {
            m_tradfiWritable = !perm.readOnly;
            break;
        }
    }

    // Restore the simulated account and its open positions for this key.
    refreshSimBalance(0.0);
    refreshSimPositions(0.0);

    // Real account holdings: fetch once immediately (before the engine is
    // started) and keep the table current with a 1 s refresh. A read-only key
    // can display positions even though it cannot modify them. The fetch is
    // attempted even when the login's CFD probe failed — that probe may have
    // lost a transient network round trip, and a genuine permission error is
    // shown in the table instead.
    showRealPositionsMessage(QStringLiteral("读取中..."));
    m_realPositionsTimer.setInterval(1'000);
    connect(&m_realPositionsTimer, &QTimer::timeout, this,
            &SimGateSystem::refreshRealPositions);
    m_realPositionsTimer.start();
    refreshRealPositions();

    // Open the SQLite order store (schema is idempotent). A failure only
    // disables the order history — paper trading keeps running.
    if (pulse::ui::SimOrderDb::init())
    {
        appendLog(QStringLiteral("[模拟] 订单数据库就绪：%1")
                      .arg(pulse::ui::SimOrderDb::defaultPath()));
    }
    else
    {
        appendLog(QStringLiteral("[模拟] 订单数据库初始化失败（SQLite 驱动或文件不可用），"
                                 "订单历史将不落库"));
    }

    appendLog(QStringLiteral("[引擎] 就绪：点击“启动引擎”开始轮询 XAUUSD 行情并运行策略。"));

    // Populate the 策略号 combo from the same roster that feeds the startup
    // log. itemData carries the strategy number, the text is "策略 N：名称".
    // Signals are blocked while filling: the initial selection must not be
    // reported as a user switch.
    {
        const QSignalBlocker blocker(ui->combo_goldTraderStrategy);
        for (const GoldTraderStrategyInfo &strategy : goldTraderStrategies())
        {
            ui->combo_goldTraderStrategy->addItem(
                QStringLiteral("策略 %1：%2").arg(strategy.number).arg(strategy.name),
                strategy.number);
        }
    }
    logGoldTraderRoster();

    // Once-per-minute XAU/USIDX/XBRUSD relation report. The timer is started
    // and stopped with the engine polling (see start/stopEnginePolling), so
    // it never runs while idle.
    m_relationTimer.setInterval(60'000);
    connect(&m_relationTimer, &QTimer::timeout, this,
            &SimGateSystem::reportMarketRelations);
}

// ---------------------------------------------------------------------------
// 策略参数 — hot-applied atomics, no engine restart needed
// ---------------------------------------------------------------------------

void SimGateSystem::on_pushBtn_applyParams_clicked()
{
    applyParamsFromUi();
    appendLog(QStringLiteral("[参数] 已应用：TP×%1 SL×%2 手数%3 杠杆%4x 冷却%5s")
                  .arg(ui->spin_tpMult->value(), 0, 'f', 1)
                  .arg(ui->spin_slMult->value(), 0, 'f', 1)
                  .arg(ui->spin_openVolume->value(), 0, 'f', 2)
                  .arg(ui->spin_leverage->value())
                  .arg(ui->spin_cooldown->value()));
}

void SimGateSystem::applyParamsFromUi()
{
    // Atomic stores — visible to the polling thread on its next read.
    m_strategyParams->tpPoints.store(ui->spin_tpMult->value());
    m_strategyParams->slPoints.store(ui->spin_slMult->value());
    m_strategyParams->openVolumeLots.store(ui->spin_openVolume->value());
    m_strategyParams->openLeverage.store(static_cast<double>(ui->spin_leverage->value()));
    m_strategyParams->cooldownMs.store(static_cast<std::int64_t>(ui->spin_cooldown->value()) * 1000);
    m_strategyParams->openProtectMin.store(ui->spin_protectMin->value());
    // The real-position automatic stops are managed by autoManageRealStops()
    // on the UI thread and read the 自动止损 controls directly; they do not go
    // through the engine's Params.
}

SimGateSystem::~SimGateSystem()
{
    // Never leave the poll thread behind a closing window.
    stopEnginePolling();
    delete ui;
}

// ---------------------------------------------------------------------------
// 模拟账户 — create on first use, edit afterwards; persisted per API key
// ---------------------------------------------------------------------------

void SimGateSystem::on_pushBtn_simAccount_clicked()
{
    const auto existing = SimAccountStore::load(m_apiKey);

    // First use: initial-balance setup. Later clicks: edit the balance.
    const QString title = existing.has_value()
                              ? QStringLiteral("修改模拟账户余额")
                              : QStringLiteral("初始余额设置");
    const double current = existing.has_value() ? existing->balanceUsdt : 0.0;

    bool ok = false;
    const double value = QInputDialog::getDouble(
        this, title, QStringLiteral("模拟账户余额（USDT）："), current,
        0.0, 9999999999.0, 2, &ok);
    if (false == ok)
    {
        return; // Cancelled — keep the stored balance untouched.
    }

    if (false == SimAccountStore::save(m_apiKey, value))
    {
        appendLog(QStringLiteral("[模拟账户] 保存失败：%1").arg(SimAccountStore::filePath()));
        return;
    }
    refreshSimBalance(0.0);
    appendLog(QStringLiteral("[模拟账户] 余额已设置：%1 USDT")
                  .arg(QString::number(value, 'f', 2)));
}

double SimGateSystem::floatingPnl(const pulse::ui::SimPosition &position, double price)
{
    if (price <= 0.0)
    {
        return 0.0;
    }
    const bool isLong = (position.side != QStringLiteral("short"));
    const double diff = isLong ? (price - position.entryPrice)
                               : (position.entryPrice - price);
    return diff * position.volumeLots * SimTradeEngine::kOzPerLot;
}

void SimGateSystem::refreshRealCfdBalance(double equityUsdt, double balanceUsdt)
{
    ui->label_balance->setText(
        QStringLiteral("%1  |  CFD 实时权益：%2 USD（可用 %3 USD）")
            .arg(m_spotBalanceText)
            .arg(equityUsdt, 0, 'f', 2)
            .arg(balanceUsdt, 0, 'f', 2));
}

void SimGateSystem::appendReferencePrice(const QString &symbol, double price)
{
    if (false == m_polling)
    {
        return;  // A late queued line must not land after "[引擎] 轮询已停止。"
    }

    // Feed the once-per-minute relation report. All three symbols call this
    // on the UI thread, so the state needs no locking. The first observation
    // starts the minute anchor; reportMarketRelations rolls it forward at
    // each tick and re-arms it after a stale gap.
    int relationIndex = kRelationXau;
    if (symbol == QStringLiteral("USIDX"))
    {
        relationIndex = kRelationUsidx;
    }
    else if (symbol == QStringLiteral("XBRUSD"))
    {
        relationIndex = kRelationXbrusd;
    }
    RelationState &relation = m_relationStates[relationIndex];
    relation.lastPrice = price;
    relation.lastUpdateMs = QDateTime::currentMSecsSinceEpoch();
    relation.hasPrice = true;
    if (false == relation.hasMinuteAnchor)
    {
        relation.minuteAnchor = price;
        relation.hasMinuteAnchor = true;
    }

    // Append one line per poll — a rolling tape. Deliberately NOT
    // setPlainText(): the index references are slow movers (measured
    // 2026-09-23 on USIDX: ~0.0008 per minute intraday), so consecutive
    // reads return the SAME number and replacing the text left the panel
    // looking frozen even though it refreshed every round. XAUUSD ticks
    // most rounds; the tape shows each one.
    //
    // Each tape holds the LATEST 50 lines and drops the oldest beyond that:
    // at a ~1.7 s cadence a pane would otherwise take ~2,100 lines an hour.
    // The bound lives in MainWindow.ui as QPlainTextEdit's
    // maximumBlockCount (QTextEdit has no equivalent — it would take manual
    // QTextCursor block surgery); plain text is all these panes ever show,
    // so nothing is lost by not using the richer widget.
    //
    // Quote precision follows the exchange: USIDX quotes 3 decimals, XBRUSD
    // 2 (verified live 2026-11-06: last_price "101.07"). At fewer decimals
    // the last digit would only tick over every few minutes, which would
    // reintroduce exactly the staleness this replaced.
    const int decimals = (symbol == QStringLiteral("USIDX")) ? 3 : 2;
    // Each symbol owns its own pane (top to bottom: XAUUSD, USIDX, XBRUSD);
    // the symbol prefix is kept in the line so a copied-out tape stays
    // self-describing.
    QPlainTextEdit *pane = nullptr;
    if (symbol == QStringLiteral("XAUUSD"))
    {
        pane = ui->textEdit_xau;
    }
    else if (symbol == QStringLiteral("XBRUSD"))
    {
        pane = ui->textEdit_xbrusd;
    }
    else
    {
        pane = ui->textEdit_usidx;
    }
    pane->appendPlainText(
        QStringLiteral("%1 %2:%3")
            .arg(beijingNow(), symbol)
            .arg(price, 0, 'f', decimals));
}

void SimGateSystem::refreshSimBalance(double price)
{
    const auto account = SimAccountStore::load(m_apiKey);
    if (false == account.has_value())
    {
        ui->label_simBalance->setText(QStringLiteral("未设置"));
        return;
    }
    // Equity = available balance + total floating PnL of open positions.
    double totalPnl = 0.0;
    for (const auto &p : account->positions)
    {
        totalPnl += floatingPnl(p, price);
    }
    const double equity = account->balanceUsdt + totalPnl;
    ui->label_simBalance->setText(
        QStringLiteral("可用 %1 USDT | 浮动权益 %2 USDT（浮盈 %3%4）")
            .arg(account->balanceUsdt, 0, 'f', 2)
            .arg(equity, 0, 'f', 2)
            .arg(totalPnl >= 0.0 ? QStringLiteral("+") : QString())
            .arg(totalPnl, 0, 'f', 2));
}

// ---------------------------------------------------------------------------
// 真实持仓 — live CFD positions shown in the table right after login.
// The refresh timer is independent of the engine poll: holdings and their
// floating PnL must be visible even while the strategy is stopped.
// ---------------------------------------------------------------------------

void SimGateSystem::refreshRealPositions()
{
    // Skip a tick when the previous request is still in flight (the proxy can
    // take several seconds); overlapping detached threads would only queue
    // identical reads.
    if (true == m_realPositionsFetching.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    const pulse::ExchangeConfig cfg =
        pulse::ui::VerifyWorker::makeExchangeConfig(m_apiKey, m_apiSecret);
    QPointer<SimGateSystem> self(this);
    std::thread([self, cfg]() {
        std::vector<pulse::ui::CfdPosition> positions;
        QString error;
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        const auto res = cfd.getCfdPositions();
        if (pulse::ok(res))
        {
            const auto parsed =
                pulse::ui::VerifyWorker::parseCfdPositions(pulse::value(res));
            if (parsed.has_value())
            {
                positions = *parsed;
            }
            else
            {
                error = QStringLiteral("持仓响应格式异常");
            }
        }
        else
        {
            const QString message = QString::fromStdString(pulse::error(res).message);
            // Translate the two credential/permission classes the user can
            // act on; transport errors keep Gate's own message.
            if (message.contains("403"))
            {
                error = QStringLiteral("权限不足（HTTP 403），该 Key 未开通 CFD 持仓读取权限");
            }
            else if (message.contains("401"))
            {
                error = QStringLiteral("API Key 或 Secret 无效（HTTP 401）");
            }
            else
            {
                error = message;
            }
        }
        QMetaObject::invokeMethod(
            self, [self, positions, error]() {
                if (self)
                {
                    self->onRealPositionsUpdated(positions, error);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void SimGateSystem::onRealPositionsUpdated(
    const std::vector<pulse::ui::CfdPosition> &positions, const QString &error)
{
    // The request is finished as soon as its result reaches the UI thread, so
    // the next timer tick may start a fresh one.
    m_realPositionsFetching.store(false, std::memory_order_release);

    // Feed the automatic stop manager even when the table repaint below is
    // paused by an open editor: the exchange values are still fresh. A
    // failed fetch keeps the last snapshot but marks it stale, so the
    // manager never writes a stop based on values it cannot re-confirm.
    if (error.isEmpty())
    {
        m_realPositions = positions;
        m_realPositionsUpdatedMs = QDateTime::currentMSecsSinceEpoch();
        // Drop throttle state for positions that no longer exist.
        for (auto it = m_autoStopStates.begin(); it != m_autoStopStates.end();)
        {
            const bool stillOpen =
                std::any_of(positions.begin(), positions.end(),
                            [&it](const pulse::ui::CfdPosition &position) {
                                return position.id == it->first;
                            });
            if (stillOpen)
            {
                ++it;
            }
            else
            {
                it = m_autoStopStates.erase(it);
            }
        }
    }
    else
    {
        m_realPositionsUpdatedMs = 0;
    }

    QTableWidget *table = ui->table_realPositions;

    // Never rewrite the table while an in-cell editor is open. Qt's delegate
    // pushes changed model data back into the editor, so the 1 s refresh would
    // erase whatever the user is typing in a 止盈/止损 cell. Skip this tick;
    // the next one runs as soon as the editor closes.
    const QLineEdit *editor = table->findChild<QLineEdit *>();
    if (nullptr != editor && editor->isVisible())
    {
        return;
    }

    if (false == error.isEmpty())
    {
        showRealPositionsMessage(QStringLiteral("读取失败：%1").arg(error));
        return;
    }
    if (positions.empty())
    {
        showRealPositionsMessage(QStringLiteral("无持仓"));
        return;
    }

    // Programmatic text updates must not look like user edits.
    m_updatingRealPositionsTable = true;
    table->clearSpans();

    // Rebuild the rows only when the position set changes. The per-row 平仓
    // buttons must survive the 1 s refresh — recreating them each tick would
    // swallow a click whose press and release straddle the timer.
    const int rowCount = static_cast<int>(positions.size());
    bool rebuild = (table->rowCount() != rowCount);
    if (false == rebuild)
    {
        for (int row = 0; row < rowCount; ++row)
        {
            const QTableWidgetItem *nameItem = table->item(row, 0);
            if (nullptr == nameItem
                || nameItem->data(Qt::UserRole).toString()
                       != positions[static_cast<std::size_t>(row)].id)
            {
                rebuild = true;
                break;
            }
        }
    }
    if (rebuild)
    {
        // Dropping to zero rows also removes the old cell widgets.
        table->setRowCount(0);
        table->setRowCount(rowCount);
    }

    for (int row = 0; row < rowCount; ++row)
    {
        const pulse::ui::CfdPosition &position =
            positions[static_cast<std::size_t>(row)];
        const bool isLong = (QStringLiteral("short") != position.side);

        // Text cells: created on a rebuild, updated in place otherwise. Only
        // the TP/SL cells accept editing.
        const auto setCell = [table, row, rebuild](int column, const QString &text,
                                                   Qt::Alignment alignment,
                                                   bool editable) {
            QTableWidgetItem *item = rebuild ? nullptr : table->item(row, column);
            if (nullptr == item)
            {
                item = new QTableWidgetItem();
                item->setTextAlignment(alignment);
                table->setItem(row, column, item);
            }
            item->setText(text);
            if (editable)
            {
                item->setFlags(item->flags() | Qt::ItemIsEditable);
            }
            else
            {
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            }
            return item;
        };

        // The symbol alone keeps the name column short; the direction has its
        // own column because a hedged account can hold the same symbol both
        // ways.
        QTableWidgetItem *nameItem =
            setCell(0, position.symbol, Qt::AlignLeft | Qt::AlignVCenter, false);
        nameItem->setData(Qt::UserRole, position.id);
        nameItem->setToolTip(QStringLiteral("持仓 ID：%1").arg(position.id));

        setCell(1, isLong ? QStringLiteral("多仓") : QStringLiteral("空仓"),
                Qt::AlignCenter, false);
        setCell(2, QString::number(position.volumeLots, 'f', 2),
                Qt::AlignRight | Qt::AlignVCenter, false);
        setCell(3, formatPositionPrice(position.entryPrice),
                Qt::AlignRight | Qt::AlignVCenter, false);
        setCell(4, formatPositionPrice(position.markPrice),
                Qt::AlignRight | Qt::AlignVCenter, false);
        // Exchange-native protective stops; a position without a TP/SL keeps
        // these cells empty rather than showing a misleading 0.00. Both cells
        // are editable: a committed value is PUT to the real position.
        //
        // While this position's PUT is in flight, leave the cells showing the
        // values the user just committed instead of snapping back to the
        // exchange's pre-modify snapshot.
        const bool modifying =
            (m_modifyingPositionIds.end() != m_modifyingPositionIds.find(position.id));
        if (rebuild || false == modifying)
        {
            QTableWidgetItem *tpItem =
                setCell(kTakeProfitColumn,
                        (0.0 < position.priceTp)
                            ? formatPositionPrice(position.priceTp)
                            : QString(),
                        Qt::AlignRight | Qt::AlignVCenter, true);
            tpItem->setData(Qt::UserRole, position.priceTp);
            QTableWidgetItem *slItem =
                setCell(kStopLossColumn,
                        (0.0 < position.priceSl)
                            ? formatPositionPrice(position.priceSl)
                            : QString(),
                        Qt::AlignRight | Qt::AlignVCenter, true);
            slItem->setData(Qt::UserRole, position.priceSl);
        }
        QTableWidgetItem *pnlItem =
            setCell(7, formatSignedUsd(position.unrealizedPnl),
                    Qt::AlignRight | Qt::AlignVCenter, false);
        // Chinese market convention: profit red, loss green. Clearing the
        // role restores the default text colour when the PnL is exactly zero.
        if (0.0 < position.unrealizedPnl)
        {
            pnlItem->setForeground(QColor(QStringLiteral("#c62828")));
        }
        else if (0.0 > position.unrealizedPnl)
        {
            pnlItem->setForeground(QColor(QStringLiteral("#2e7d32")));
        }
        else
        {
            pnlItem->setData(Qt::ForegroundRole, QVariant());
        }

        const bool closing =
            (m_closingPositionIds.end() != m_closingPositionIds.find(position.id));
        if (rebuild)
        {
            auto *closeButton = new QPushButton(
                closing ? QStringLiteral("平仓中") : QStringLiteral("平仓"));
            closeButton->setEnabled(false == closing);
            connect(closeButton, &QPushButton::clicked, this,
                    [this, position]() { closeRealPosition(position); });
            table->setCellWidget(row, kRealPositionCloseColumn, closeButton);
        }
        else
        {
            auto *closeButton = qobject_cast<QPushButton *>(
                table->cellWidget(row, kRealPositionCloseColumn));
            if (nullptr != closeButton)
            {
                closeButton->setText(closing ? QStringLiteral("平仓中")
                                             : QStringLiteral("平仓"));
                closeButton->setEnabled(false == closing);
            }
        }
    }
    m_updatingRealPositionsTable = false;
}

void SimGateSystem::showRealPositionsMessage(const QString &text)
{
    QTableWidget *table = ui->table_realPositions;
    m_updatingRealPositionsTable = true;
    table->clearSpans();
    // Drop the per-row 平仓 buttons together with the data rows.
    for (int row = 0; row < table->rowCount(); ++row)
    {
        table->removeCellWidget(row, kRealPositionCloseColumn);
    }
    table->setRowCount(0);
    table->setRowCount(1);
    table->setSpan(0, 0, 1, table->columnCount());
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setToolTip(text); // long transport errors are clipped by the row
    table->setItem(0, 0, item);
    m_updatingRealPositionsTable = false;
}

void SimGateSystem::onRealPositionItemChanged(QTableWidgetItem *item)
{
    if (m_updatingRealPositionsTable || nullptr == item)
    {
        return;
    }
    const int column = item->column();
    if (kTakeProfitColumn != column && kStopLossColumn != column)
    {
        return;
    }

    QTableWidget *table = ui->table_realPositions;
    QTableWidgetItem *nameItem = table->item(item->row(), 0);
    if (nullptr == nameItem)
    {
        return;
    }
    const QString id = nameItem->data(Qt::UserRole).toString();
    if (id.isEmpty())
    {
        return;
    }

    // Reject a malformed value and restore the last accepted one. Empty is a
    // VALID edit: it clears that protective price on the exchange ("0").
    const QString text = item->text().trimmed();
    bool parsed = false;
    const double newPrice = text.isEmpty() ? 0.0 : text.toDouble(&parsed);
    if (false == text.isEmpty() && (false == parsed || 0.0 >= newPrice))
    {
        const double previous = item->data(Qt::UserRole).toDouble();
        m_updatingRealPositionsTable = true;
        item->setText((0.0 < previous) ? formatPositionPrice(previous)
                                       : QString());
        m_updatingRealPositionsTable = false;
        QMessageBox::warning(
            this, QStringLiteral("价格无效"),
            QStringLiteral("请输入大于 0 的价格，或留空以清除该价格。"));
        return;
    }

    // Don't stack writes on one position — the second PUT could reach Gate
    // before the first and leave the older stops attached. An automatic stop
    // update holds the same row.
    if (m_modifyingPositionIds.end() != m_modifyingPositionIds.find(id)
        || m_autoStopInFlight.end() != m_autoStopInFlight.find(id))
    {
        const double previous = item->data(Qt::UserRole).toDouble();
        m_updatingRealPositionsTable = true;
        item->setText((0.0 < previous) ? formatPositionPrice(previous)
                                       : QString());
        m_updatingRealPositionsTable = false;
        appendLog(QStringLiteral("[真实持仓] %1 的止盈止损正在同步，请稍候再改")
                      .arg(nameItem->text()));
        return;
    }

    // Accept the edit locally, canonicalise the cell and remember the value,
    // so an edit of the OTHER price sends this one along. `previous` is the
    // exchange value from the last refresh — needed to spot a no-op edit.
    const double previous = item->data(Qt::UserRole).toDouble();
    const bool unchanged = std::abs(newPrice - previous) <= 1e-9;
    m_updatingRealPositionsTable = true;
    item->setText((0.0 < newPrice) ? formatPositionPrice(newPrice) : QString());
    item->setData(Qt::UserRole, newPrice);
    m_updatingRealPositionsTable = false;

    // Gate rejects a modification that does not change either price with
    // HTTP 400 INVALID_ARGUMENT ("Invalid parameter"). Committing the same
    // value (or clearing an already-empty cell) must therefore stay local.
    if (unchanged)
    {
        appendLog(QStringLiteral("[真实持仓] %1 的%2价格未变化，未提交")
                      .arg(nameItem->text(),
                           (kTakeProfitColumn == column) ? QStringLiteral("止盈")
                                                         : QStringLiteral("止损")));
        return;
    }

    QTableWidgetItem *tpItem = table->item(item->row(), kTakeProfitColumn);
    QTableWidgetItem *slItem = table->item(item->row(), kStopLossColumn);
    if (nullptr == tpItem || nullptr == slItem)
    {
        return;
    }
    const double tp = tpItem->data(Qt::UserRole).toDouble();
    const double sl = slItem->data(Qt::UserRole).toDouble();
    const QString symbol = nameItem->text();

    m_modifyingPositionIds.insert(id);
    appendLog(QStringLiteral("[真实持仓] %1 提交止盈/止损：TP %2 / SL %3")
                  .arg(symbol,
                       (0.0 < tp) ? formatPositionPrice(tp) : QStringLiteral("无"),
                       (0.0 < sl) ? formatPositionPrice(sl) : QStringLiteral("无")));

    const pulse::ExchangeConfig cfg =
        pulse::ui::VerifyWorker::makeExchangeConfig(m_apiKey, m_apiSecret);
    // Both fields are always sent; "0" clears one on Gate's side.
    const QString tpPayload = stopPricePayload(tp);
    const QString slPayload = stopPricePayload(sl);
    QPointer<SimGateSystem> self(this);
    std::thread([self, cfg, id, symbol, tp, sl, tpPayload, slPayload]() {
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        // Argument order is (id, SL, TP).
        const auto res = cfd.putCfdPositionModify(
            id.toStdString(), slPayload.toStdString(), tpPayload.toStdString());
        const bool ok = pulse::ok(res);
        const QString error =
            ok ? QString() : QString::fromStdString(pulse::error(res).message);
        QMetaObject::invokeMethod(
            self, [self, id, symbol, tp, sl, ok, error]() {
                if (self)
                {
                    self->onRealStopsSynced(id, symbol, tp, sl, ok, error);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void SimGateSystem::onRealStopsSynced(const QString &positionId,
                                      const QString &symbol, double tp, double sl,
                                      bool ok, const QString &error)
{
    m_modifyingPositionIds.erase(positionId);
    const QString tpText =
        (0.0 < tp) ? formatPositionPrice(tp) : QStringLiteral("无");
    const QString slText =
        (0.0 < sl) ? formatPositionPrice(sl) : QStringLiteral("无");
    if (ok)
    {
        const QString text = QStringLiteral("%1 止盈 %2 / 止损 %3 已同步到 Gate")
                                 .arg(symbol, tpText, slText);
        appendTrade(QStringLiteral("[止盈止损] %1").arg(text));
        appendLog(QStringLiteral("[真实持仓] %1").arg(text));
    }
    else
    {
        const QString text =
            QStringLiteral("%1 止盈止损同步失败：%2").arg(symbol, error);
        appendLog(QStringLiteral("[真实持仓] %1").arg(text));
        QMessageBox::warning(this, QStringLiteral("同步失败"), text);
    }
    // Re-read the exchange values: a failure restores the true stops, and a
    // success confirms exactly what Gate accepted.
    refreshRealPositions();
}

void SimGateSystem::closeRealPosition(const pulse::ui::CfdPosition &position,
                                      bool silent)
{
    if (m_closingPositionIds.end() != m_closingPositionIds.find(position.id))
    {
        return; // a close for this position is already in flight
    }

    // One click = REAL market close: POST /tradfi/positions/{id}/close with
    // close_type 2 (full). No confirmation dialog — direct close, as
    // requested. The in-flight set below prevents a second click from
    // stacking a duplicate request.
    // Show the request as in flight before it leaves the UI thread.
    m_closingPositionIds.insert(position.id);
    QTableWidget *table = ui->table_realPositions;
    for (int row = 0; row < table->rowCount(); ++row)
    {
        const QTableWidgetItem *nameItem = table->item(row, 0);
        if (nullptr == nameItem
            || nameItem->data(Qt::UserRole).toString() != position.id)
        {
            continue;
        }
        auto *button = qobject_cast<QPushButton *>(
            table->cellWidget(row, kRealPositionCloseColumn));
        if (nullptr != button)
        {
            button->setText(QStringLiteral("平仓中"));
            button->setEnabled(false);
        }
        break;
    }

    const pulse::ExchangeConfig cfg =
        pulse::ui::VerifyWorker::makeExchangeConfig(m_apiKey, m_apiSecret);
    const QString id = position.id;
    const QString symbol = position.symbol;
    QPointer<SimGateSystem> self(this);
    std::thread([self, cfg, id, symbol, silent]() {
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        // close_type 2 = full close (the endpoint takes no volume).
        const auto res = cfd.postCfdPositionClose(id.toStdString(), 2);
        const bool ok = pulse::ok(res);
        const QString error =
            ok ? QString() : QString::fromStdString(pulse::error(res).message);
        QMetaObject::invokeMethod(
            self, [self, id, symbol, ok, error, silent]() {
                if (self)
                {
                    self->onRealPositionClosed(id, symbol, ok, error, silent);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void SimGateSystem::onRealPositionClosed(const QString &positionId,
                                         const QString &symbol, bool ok,
                                         const QString &error, bool silent)
{
    m_closingPositionIds.erase(positionId);
    if (ok)
    {
        // Drop it from the local snapshot now: the confirming GET is still in
        // flight, and the unattended peak-drawdown path must not see this row
        // and fire a second close against a position that is already gone.
        m_realPositions.erase(
            std::remove_if(m_realPositions.begin(), m_realPositions.end(),
                           [&positionId](const pulse::ui::CfdPosition &p) {
                               return p.id == positionId;
                           }),
            m_realPositions.end());
        m_autoStopStates.erase(positionId);

        const QString text = QStringLiteral("%1 已市价全平（持仓 ID %2）")
                                 .arg(symbol, positionId);
        appendTrade(QStringLiteral("[平仓] %1").arg(text));
        appendLog(QStringLiteral("[真实持仓] %1").arg(text));
    }
    else
    {
        const QString text =
            QStringLiteral("%1 平仓失败：%2").arg(symbol, error);
        appendLog(QStringLiteral("[真实持仓] %1").arg(text));
        if (false == silent)
        {
            QMessageBox::warning(this, QStringLiteral("平仓失败"), text);
        }
    }
    // Re-read now; if a timed fetch is already in flight it is skipped and
    // that request (or the next 1 s tick) refreshes the row and its button.
    refreshRealPositions();
}

// ---------------------------------------------------------------------------
// 自动止损 — manage the LIVE XAUUSD stops from the 1 m SuperTrend.
//
// Policy (requested 2026-11-12):
//   * floating PnL < 0               → 8U loss stop (8 USD / (lots × 100))
//   * floating PnL > +2 USD          → break-even stop: entry + 0.2 (long) /
//                                      entry − 0.2 (short)
//   * 0 ≤ PnL ≤ +2 with no stop yet  → the 8U stop as a protection floor
//   * every positive PnL sample      → peak-PnL trail: the stop price for
//                                      (max PnL − 回撤U) competes with the
//                                      8U / break-even / SuperTrend stops;
//                                      the tightest one wins
//   * drawdown from peak ≥ 回撤U     → immediate market close (the trail stop
//                                      can no longer be placed)
//
// A stop is never loosened: the stop already on the exchange wins whenever it
// is tighter. Every candidate is also compared with the 1 m SuperTrend line
// placed 1U beyond the line in the loss direction (room against pin-bar
// wicks) — if that buffered stop is still better (tighter) than the 8U /
// break-even / peak stop and the line sits on the protective side of the
// market, it becomes the stop. Writes are throttled per position and skipped
// when nothing changed — Gate rejects no-op modifications with HTTP 400.
// ---------------------------------------------------------------------------

void SimGateSystem::autoManageRealStops(double price,
                                        const pulse::ui::TickOutcome &outcome)
{
    if (false == ui->check_goldTrader->isChecked()
        || false == ui->check_autoFollow->isChecked() || false == m_tradfiWritable)
    {
        return;
    }
    // Strategy dispatch: only number 1 is implemented today. A future roster
    // entry gets its own branch here; an unknown number does nothing rather
    // than silently running another strategy's rules.
    if (1 != selectedGoldTraderStrategy())
    {
        return;
    }
    if (0.0 >= price || m_realPositions.empty() || 0 == m_realPositionsUpdatedMs)
    {
        return;
    }

    const std::int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
    // Act only on a fresh position snapshot: the 1 s refresh normally renews
    // it, and a network outage must freeze automatic writes instead of
    // letting them run on stale stops/PnL.
    if (nowMs - m_realPositionsUpdatedMs > 10'000)
    {
        return;
    }

    // The 1 m SuperTrend line. Other timeframes are display-only here.
    const pulse::ui::TrendInfo *trend1m = nullptr;
    for (const auto &trend : outcome.trends)
    {
        if (trend.valid && QLatin1String("1m") == trend.label)
        {
            trend1m = &trend;
            break;
        }
    }

    const std::int64_t intervalMs =
        static_cast<std::int64_t>(ui->spin_followInterval->value()) * 1000;
    const double minDelta = ui->spin_followThreshold->value();
    // Maximum giveback from the highest floating PnL (USD). The equivalent
    // stop price competes with the tier/SuperTrend stops; once the giveback
    // is fully used, the position is closed at market.
    const double givebackU = ui->spin_peakGiveback->value();

    for (auto &position : m_realPositions)
    {
        if (QStringLiteral("XAUUSD") != position.symbol || position.id.isEmpty()
            || 0.0 >= position.volumeLots || 0.0 >= position.entryPrice)
        {
            continue;
        }

        const bool isLong = (QStringLiteral("short") != position.side);
        // Use the live polled tick (the same price the SuperTrend was
        // evaluated on); the snapshot's mark is up to 1 s older.
        const double mark = (0.0 < price) ? price : position.markPrice;
        if (mark <= 0.0)
        {
            continue;
        }

        // 0.01 lot of XAUUSD = 1 oz → one full price unit is (lots × 100) USD.
        const double usdPerPrice = position.volumeLots * 100.0;
        const double livePnl = isLong ? (mark - position.entryPrice) * usdPerPrice
                                      : (position.entryPrice - mark) * usdPerPrice;

        // Per-position state. The peak is updated BEFORE the write throttle
        // and in-flight guards: the maximum PnL is sampled every second even
        // while a previous stop write is still running.
        AutoStopState &state = m_autoStopStates[position.id];
        if (0.0 < livePnl && livePnl > state.peakPnl)
        {
            state.peakPnl = livePnl;
            state.hasPositivePeak = true;
        }

        // The peak trail is checked before the write throttle: once the
        // position has given back the full allowance, the trail stop price is
        // at or through the market and cannot be placed — close at market now.
        if (state.hasPositivePeak && state.peakPnl - livePnl >= givebackU)
        {
            if (m_closingPositionIds.end() == m_closingPositionIds.find(position.id)
                && (0 == state.lastPeakCloseMs
                    || nowMs - state.lastPeakCloseMs >= 5'000))
            {
                state.lastPeakCloseMs = nowMs;
                appendLog(
                    QStringLiteral("[峰值回撤] %1 浮盈从最高 %2U 回撤到 %3U"
                                   "（回撤 %4U ≥ %5U），立即市价平仓。")
                        .arg(position.symbol)
                        .arg(state.peakPnl, 0, 'f', 2)
                        .arg(livePnl, 0, 'f', 2)
                        .arg(state.peakPnl - livePnl, 0, 'f', 2)
                        .arg(givebackU, 0, 'f', 2));
                closeRealPosition(position, true);
            }
            continue;
        }

        // One write per position at a time. A manual TP/SL edit or a close
        // also owns the row until its request completes.
        if (m_autoStopInFlight.end() != m_autoStopInFlight.find(position.id)
            || m_modifyingPositionIds.end() != m_modifyingPositionIds.find(position.id)
            || m_closingPositionIds.end() != m_closingPositionIds.find(position.id))
        {
            continue;
        }

        if (0 != state.lastAttemptMs && nowMs - state.lastAttemptMs < intervalMs)
        {
            continue;
        }

        const double lossStop = isLong
                                    ? position.entryPrice - 8.0 / usdPerPrice
                                    : position.entryPrice + 8.0 / usdPerPrice;
        // 保本损: lock in 0.2 price units (0.2 USD per 0.01 lot of gold).
        const double breakEvenStop = isLong ? position.entryPrice + 0.2
                                            : position.entryPrice - 0.2;

        // The tier stop is the starting candidate. The peak trail and the
        // 1 m SuperTrend then compete with it; the most protective one wins.
        std::optional<double> candidate;
        QString reason;
        if (0.0 > position.unrealizedPnl)
        {
            candidate = lossStop;
            reason = QStringLiteral("8U 亏损止损");
        }
        else if (2.0 < position.unrealizedPnl)
        {
            candidate = breakEvenStop;
            reason = QStringLiteral("保本损");
        }
        else if (0.0 >= position.priceSl)
        {
            // Between the two requested tiers but still unprotected: give
            // the position the 8U loss cap as a floor.
            candidate = lossStop;
            reason = QStringLiteral("8U 保护止损");
        }
        // else: 0..+2 USD with a stop already attached — no tier candidate,
        // but the peak/ST candidates below may still tighten it.

        // Peak-PnL trail: stop price equivalent to (max PnL − givebackU).
        // Only active once the position has actually been in profit. It is
        // compared with the best tier stop: long → higher wins, short → lower.
        if (state.hasPositivePeak)
        {
            const double peakStop =
                isLong ? position.entryPrice + (state.peakPnl - givebackU) / usdPerPrice
                       : position.entryPrice - (state.peakPnl - givebackU) / usdPerPrice;
            if (false == candidate.has_value()
                || (isLong ? peakStop > *candidate : peakStop < *candidate))
            {
                candidate = peakStop;
                reason = QStringLiteral("峰值回撤 %1U（最高 %2U）")
                             .arg(givebackU, 0, 'f', 2)
                             .arg(state.peakPnl, 0, 'f', 2);
            }
        }

        // The 1 m SuperTrend competes with EVERY candidate — including the
        // 8U loss stop right after opening. The line is given 1U of extra
        // room in the loss direction (the user's "再多亏损 1U"), and the
        // resulting stop is used when it is still better (tighter):
        //   long  → higher is better (less loss / more locked profit)
        //   short → lower is better.
        if (nullptr != trend1m && 0.0 < trend1m->supertrend
            && (isLong ? trend1m->supertrend < mark
                       : trend1m->supertrend > mark))
        {
            const double stBuffer = 1.0 / usdPerPrice; // 1 USD in price units
            const double trendStop = isLong ? trend1m->supertrend - stBuffer
                                            : trend1m->supertrend + stBuffer;
            if (false == candidate.has_value()
                || (isLong ? trendStop > *candidate : trendStop < *candidate))
            {
                candidate = trendStop;
                reason = QStringLiteral("1m 超级趋势（多留 1U）");
            }
        }

        if (false == candidate.has_value())
        {
            continue;
        }

        // Snap to XAUUSD's 2-decimal quote precision before comparing against
        // the exchange values (see roundXauPrice).
        double target = roundXauPrice(*candidate);

        // Never loosen an existing protection.
        if (0.0 < position.priceSl
            && (isLong ? position.priceSl > target
                       : position.priceSl < target))
        {
            continue;
        }

        // Gate requires the stop to sit on the protective side of the market
        // and not too close to it (XAUUSD price_sl_level = 50 points = 0.50).
        constexpr double kMinStopDistance = 0.5;
        if (isLong ? target > mark - kMinStopDistance
                   : target < mark + kMinStopDistance)
        {
            // The 8U level is already reached/crossed, or 8U is closer to the
            // market than Gate's minimum stop distance (a large position): it
            // cannot be placed there. Say so, but only once a minute — this
            // state persists until the price recovers.
            if (reason.startsWith(QStringLiteral("8U")))
            {
                state.lastAttemptMs = nowMs;
                if (0 == state.lastWarningMs
                    || nowMs - state.lastWarningMs >= 60'000)
                {
                    state.lastWarningMs = nowMs;
                    appendLog(
                        QStringLiteral("[自动止损] %1 无法挂出 8U 止损：目标价 %2 "
                                       "距市价不足 0.50 或已在市价错误一侧"
                                       "（亏损接近/超过 8U，或手数过大）。")
                            .arg(position.symbol, formatPositionPrice(lossStop)));
                }
            }
            continue;
        }

        // A just-sent target may take a moment to appear in the 1 s position
        // refresh; do not mistake that lag for a missing stop and resend it.
        if (0.0 < state.lastSentSl && nowMs - state.lastSentMs < 15'000
            && std::abs(target - state.lastSentSl) <= 1e-9)
        {
            continue;
        }
        if (std::abs(target - position.priceSl) <= 1e-6)
        {
            continue; // already exactly where we want it — Gate rejects no-ops.
        }
        // Sub-threshold trailing moves are noise; the fixed 8U/保本 tiers
        // are always applied when they first become relevant.
        if ((reason.startsWith(QStringLiteral("1m 超级趋势"))
             || reason.startsWith(QStringLiteral("峰值回撤")))
            && 0.0 < position.priceSl
            && std::abs(target - position.priceSl) < minDelta)
        {
            continue;
        }

        state.lastAttemptMs = nowMs;
        submitAutoRealStop(position, target, reason);
    }
}

void SimGateSystem::submitAutoRealStop(const pulse::ui::CfdPosition &position,
                                       double stopPrice, const QString &reason)
{
    const QString id = position.id;
    const QString symbol = position.symbol;
    // Gate takes both protective prices; omitting the TP would CLEAR it, so
    // carry the position's current TP through every automatic SL write.
    const QString slPayload = stopPricePayload(stopPrice);
    const QString tpPayload = stopPricePayload(position.priceTp);

    m_autoStopInFlight.insert(id);
    AutoStopState &state = m_autoStopStates[id];
    state.lastSentSl = stopPrice;
    state.lastSentMs = QDateTime::currentMSecsSinceEpoch();

    const pulse::ExchangeConfig cfg =
        pulse::ui::VerifyWorker::makeExchangeConfig(m_apiKey, m_apiSecret);
    QPointer<SimGateSystem> self(this);
    std::thread([self, cfg, id, symbol, stopPrice, reason, slPayload, tpPayload]() {
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        // Argument order is (id, SL, TP).
        const auto res = cfd.putCfdPositionModify(
            id.toStdString(), slPayload.toStdString(), tpPayload.toStdString());
        const bool ok = pulse::ok(res);
        const QString error =
            ok ? QString() : QString::fromStdString(pulse::error(res).message);
        QMetaObject::invokeMethod(
            self, [self, id, symbol, stopPrice, reason, ok, error]() {
                if (self)
                {
                    self->onAutoRealStopSynced(id, symbol, stopPrice, reason, ok,
                                               error);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void SimGateSystem::onAutoRealStopSynced(const QString &positionId,
                                         const QString &symbol, double stopPrice,
                                         const QString &reason, bool ok,
                                         const QString &error)
{
    m_autoStopInFlight.erase(positionId);
    const auto stateIt = m_autoStopStates.find(positionId);
    if (stateIt != m_autoStopStates.end() && false == ok)
    {
        // Allow a retry (after the throttle) with the same target.
        stateIt->second.lastSentSl = 0.0;
        stateIt->second.lastSentMs = 0;
    }

    if (ok)
    {
        // Mirror the accepted value into the local snapshot immediately: the
        // next 1 s GET confirms it, and until then the manager must not see
        // the pre-modify stop and submit the same target again.
        for (auto &position : m_realPositions)
        {
            if (position.id == positionId)
            {
                position.priceSl = stopPrice;
                break;
            }
        }
        const QString text = QStringLiteral("%1 止损已自动更新为 %2（%3）")
                                 .arg(symbol, formatPositionPrice(stopPrice), reason);
        appendTrade(QStringLiteral("[自动止损] %1").arg(text));
        appendLog(QStringLiteral("[自动止损] %1").arg(text));
    }
    else
    {
        // Unattended path: log only, never a modal dialog.
        appendLog(QStringLiteral("[自动止损] %1 止损更新失败（目标 %2，%3）：%4")
                      .arg(symbol, formatPositionPrice(stopPrice), reason, error));
    }
}

// ---------------------------------------------------------------------------
// 动态止盈止损 — LIVE positions: query the real CFD positions, then apply
// the strategy's TP/SL (current price ± points) via putCfdPositionModify.
// This is a REAL write operation — never touches the paper account.
// ---------------------------------------------------------------------------

void SimGateSystem::on_pushBtn_dynamicStops_clicked()
{
    // Write-permission gate: modifying CFD stops is a write operation — a
    // read-only key would be rejected by the real exchange.
    if (false == m_tradfiWritable)
    {
        appendTrade(QStringLiteral("[止损] 当前 Key 对 CFD（黄金）仅有只读权限，"
                                   "无法修改真实持仓止盈止损（Gate 将拒绝）。"
                                   "如需此功能，请在 Gate 后台为该 Key 开启读写权限后重新登录。"));
        return;
    }
    if (m_lastPrice <= 0.0)
    {
        appendTrade(QStringLiteral("[止损] 暂无行情价格：请先启动引擎获取实时价格。"));
        return;
    }

    appendTrade(QStringLiteral("[止损] 正在查询真实 CFD 持仓..."));
    fetchRealPositions();
}

void SimGateSystem::fetchRealPositions()
{
    // Background REST query — never block the UI thread.
    const pulse::ExchangeConfig cfg =
        pulse::ui::VerifyWorker::makeExchangeConfig(m_apiKey, m_apiSecret);
    QPointer<SimGateSystem> self(this);
    std::thread([self, cfg]() {
        std::vector<pulse::ui::CfdPosition> positions;
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        const auto res = cfd.getCfdPositions();
        if (pulse::ok(res))
        {
            const auto parsed =
                pulse::ui::VerifyWorker::parseCfdPositions(pulse::value(res));
            if (parsed.has_value())
            {
                // Only gold positions get the strategy's gold stops.
                for (const auto &position : *parsed)
                {
                    if (QStringLiteral("XAUUSD") == position.symbol)
                    {
                        positions.push_back(position);
                    }
                }
            }
        }
        QMetaObject::invokeMethod(
            self, [self, positions]() {
                if (self)
                {
                    self->onRealPositionsFetched(positions);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void SimGateSystem::onRealPositionsFetched(const std::vector<pulse::ui::CfdPosition> &positions)
{
    if (positions.empty())
    {
        appendTrade(QStringLiteral("[止损] 真实账户当前没有 XAUUSD 持仓，无需调整。"));
        return;
    }

    // Confirmation with the planned stops — this is a REAL write. The stops
    // are anchored on each position's ENTRY price (entry ± points).
    const double tpPoints = m_strategyParams->tpPoints.load();
    const double slPoints = m_strategyParams->slPoints.load();

    QString detail;
    for (const auto &pos : positions)
    {
        const bool isLong = (pos.side != QStringLiteral("short"));
        const double tp = isLong ? pos.entryPrice + tpPoints : pos.entryPrice - tpPoints;
        const double sl = isLong ? pos.entryPrice - slPoints : pos.entryPrice + slPoints;
        detail += QStringLiteral("  %1 %2 %3手 开仓%4 → TP %5 / SL %6\n")
                      .arg(QStringLiteral("XAUUSD"),
                           isLong ? QStringLiteral("多") : QStringLiteral("空"))
                      .arg(pos.volumeLots, 0, 'f', 2)
                      .arg(pos.entryPrice, 0, 'f', 2)
                      .arg(tp, 0, 'f', 2)
                      .arg(sl, 0, 'f', 2);
    }
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, QStringLiteral("确认动态止盈止损"),
        QStringLiteral("将对以下真实 CFD 持仓按开仓价设置止盈止损\n"
                       "（TP/SL ±%1/%2 点，锚定开仓价）——写操作：\n\n%3\n确认执行？")
            .arg(tpPoints, 0, 'f', 1)
            .arg(slPoints, 0, 'f', 1)
            .arg(detail),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (QMessageBox::Yes != choice)
    {
        appendTrade(QStringLiteral("[止损] 已取消（未修改任何真实订单）。"));
        return;
    }
    applyRealStops(positions);
}

void SimGateSystem::applyRealStops(const std::vector<pulse::ui::CfdPosition> &positions)
{
    const double tpPoints = m_strategyParams->tpPoints.load();
    const double slPoints = m_strategyParams->slPoints.load();

    const pulse::ExchangeConfig cfg =
        pulse::ui::VerifyWorker::makeExchangeConfig(m_apiKey, m_apiSecret);
    QPointer<SimGateSystem> self(this);
    std::thread([self, cfg, positions, tpPoints, slPoints]() {
        std::vector<pulse::ui::RealStopsResult> results;
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        for (const auto &pos : positions)
        {
            const bool isLong = (pos.side != QStringLiteral("short"));
            // Anchored on the position's entry price.
            const double tp = isLong ? pos.entryPrice + tpPoints : pos.entryPrice - tpPoints;
            const double sl = isLong ? pos.entryPrice - slPoints : pos.entryPrice + slPoints;
            pulse::ui::RealStopsResult r;
            r.id = pos.id;
            // Gate rejects a modification that changes neither price
            // (HTTP 400 INVALID_ARGUMENT), so an already-matching position is
            // reported as applied without a write.
            if (std::abs(pos.priceSl - sl) <= 1e-9
                && std::abs(pos.priceTp - tp) <= 1e-9)
            {
                r.ok = true;
                results.push_back(r);
                continue;
            }
            const auto res = cfd.putCfdPositionModify(
                pos.id.toStdString(), std::to_string(sl), std::to_string(tp));
            r.ok = pulse::ok(res);
            if (false == r.ok)
            {
                r.error = QString::fromStdString(pulse::error(res).message);
            }
            results.push_back(r);
        }
        QMetaObject::invokeMethod(
            self, [self, results]() {
                if (self)
                {
                    self->onRealStopsApplied(results, 0.0);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void SimGateSystem::onRealStopsApplied(const std::vector<pulse::ui::RealStopsResult> &results,
                                       double price)
{
    int okCount = 0;
    for (const auto &r : results)
    {
        if (r.ok)
        {
            okCount++;
            appendTrade(QStringLiteral("[止损] 持仓 %1：止盈止损已更新（按 %2 计算）。")
                            .arg(r.id)
                            .arg(price, 0, 'f', 2));
        }
        else
        {
            appendTrade(QStringLiteral("[止损] 持仓 %1：设置失败 — %2").arg(r.id, r.error));
        }
    }
    appendLog(QStringLiteral("[止损] 真实持仓止盈止损：%1/%2 成功。")
                  .arg(okCount)
                  .arg(results.size()));
}

// ---------------------------------------------------------------------------
// 订单分析 — post-trade report from the SQLite order store
// ---------------------------------------------------------------------------

void SimGateSystem::on_pushBtn_analyze_clicked()
{
    const pulse::ui::AnalysisReport report =
        pulse::ui::SimOrderAnalyzer::analyze(m_apiKey);
    if (report.totalOrders == 0)
    {
        appendTrade(QStringLiteral("[分析] 暂无订单记录（数据库为空或不可用）"));
        return;
    }

    appendTrade(QStringLiteral("[分析] ═══ 订单分析（共 %1 笔：已平 %2，持仓中 %3）═══")
                    .arg(report.totalOrders)
                    .arg(report.closedOrders)
                    .arg(report.openOrders));

    // Per-order lifecycle.
    for (const auto &o : report.orders)
    {
        const bool closed = (false == o.closeReason.isEmpty());
        QString line = QStringLiteral("%1 %2 %3手@%4 | 开仓 %5")
                           .arg(o.orderId,
                                o.side == QStringLiteral("short")
                                    ? QStringLiteral("空")
                                    : QStringLiteral("多"))
                           .arg(o.entryPrice, 0, 'f', 2)
                           .arg(o.openedAt);
        line += QStringLiteral(" | TP %1→%2 SL %3→%4（移动 %5 次）")
                    .arg(o.tpInitial, 0, 'f', 2)
                    .arg(o.tpFinal, 0, 'f', 2)
                    .arg(o.slInitial, 0, 'f', 2)
                    .arg(o.slFinal, 0, 'f', 2)
                    .arg(o.stopMoves);
        if (closed)
        {
            line += QStringLiteral(" | %1平 %2 盈亏 %3%4")
                        .arg(o.closeReason)
                        .arg(o.exitPrice, 0, 'f', 2)
                        .arg(o.realizedPnl >= 0.0 ? QStringLiteral("+") : QString())
                        .arg(o.realizedPnl, 0, 'f', 2);
        }
        else
        {
            line += QStringLiteral(" | 持仓中");
        }
        appendTrade(line);
    }

    // Aggregates.
    const double winRate = report.closedOrders > 0
                               ? 100.0 * report.wins / report.closedOrders
                               : 0.0;
    appendTrade(QStringLiteral("[分析] 胜率 %1%%（%2 胜 / %3 负）| 总盈亏 %4%5 USDT | "
                               "TP 平 %6 笔，SL 平 %7 笔")
                    .arg(winRate, 0, 'f', 1)
                    .arg(report.wins)
                    .arg(report.losses)
                    .arg(report.totalPnl >= 0.0 ? QStringLiteral("+") : QString())
                    .arg(report.totalPnl, 0, 'f', 2)
                    .arg(report.tpCloses)
                    .arg(report.slCloses));
    appendTrade(QStringLiteral("[分析] 多单 %1 笔（盈亏 %2%3）| 空单 %4 笔（盈亏 %5%6）")
                    .arg(report.longCount)
                    .arg(report.longPnl >= 0.0 ? QStringLiteral("+") : QString())
                    .arg(report.longPnl, 0, 'f', 2)
                    .arg(report.shortCount)
                    .arg(report.shortPnl >= 0.0 ? QStringLiteral("+") : QString())
                    .arg(report.shortPnl, 0, 'f', 2));
}

// ---------------------------------------------------------------------------
// Engine polling — 启动引擎 starts THREE independent poll threads:
//   m_pollThread   — 1 s XAUUSD poll  → strategy → paper trades + XAU pane
//   m_usidxThread  — 1 s USIDX poll   → USIDX pane (display-only)
//   m_xbrusdThread — 1 s XBRUSD poll  → XBRUSD pane (display-only)
// They are launched and joined together; see startEnginePolling() for why
// the reference symbols do not simply share the gold loop.
// ---------------------------------------------------------------------------

void SimGateSystem::on_pushBtn_startEngine_clicked()
{
    if (m_polling)
    {
        appendLog(QStringLiteral("[引擎] 已在轮询中，无需重复启动。"));
        return;
    }
    startEnginePolling();
}

void SimGateSystem::on_pushBtn_stopEngine_clicked()
{
    if (false == m_polling)
    {
        appendLog(QStringLiteral("[引擎] 当前未在轮询。"));
        return;
    }
    stopEnginePolling();
}

void SimGateSystem::on_check_goldTrader_toggled(bool checked)
{
    if (checked)
    {
        const int number = selectedGoldTraderStrategy();
        QString name = QStringLiteral("未知策略");
        for (const GoldTraderStrategyInfo &strategy : goldTraderStrategies())
        {
            if (strategy.number == number)
            {
                name = strategy.name;
                break;
            }
        }
        appendLog(QStringLiteral("[黄金交易员] 已启动：执行策略 %1（%2），"
                                 "开始动态调整真实持仓止损。")
                      .arg(number)
                      .arg(name));
        if (false == m_polling)
        {
            // The strategy needs the live price and the 1 m SuperTrend, both
            // produced by the engine poll. Arm-and-start in one action.
            appendLog(QStringLiteral("[黄金交易员] 行情轮询未运行，正在自动启动引擎..."));
            startEnginePolling();
        }
    }
    else
    {
        // Disarming is a deliberate stop: forget peak/throttle state so a
        // later re-arm starts a fresh trail instead of closing on a drawdown
        // that happened while the strategy was off.
        m_autoStopStates.clear();
        appendLog(QStringLiteral("[黄金交易员] 已停止：只展示订单数据，不再自动调整止损"
                                 "（已挂在交易所的止损仍然有效）。"));
    }
}

void SimGateSystem::on_combo_goldTraderStrategy_currentIndexChanged(int index)
{
    if (0 > index)
    {
        return;
    }
    const int number = selectedGoldTraderStrategy();
    for (const GoldTraderStrategyInfo &strategy : goldTraderStrategies())
    {
        if (strategy.number == number)
        {
            appendLog(QStringLiteral("[黄金交易员] 已切换策略 %1：%2 — %3")
                          .arg(strategy.number)
                          .arg(strategy.name, strategy.summary));
            break;
        }
    }
    // Each strategy has its own thresholds and peak semantics; state gathered
    // under the previous strategy must not leak into the new one.
    m_autoStopStates.clear();
    if (ui->check_goldTrader->isChecked())
    {
        appendLog(QStringLiteral("[黄金交易员] 新策略已立即生效，峰值统计从头开始。"));
    }
}

void SimGateSystem::startEnginePolling()
{
    m_polling = true;
    m_stopRequested = false;
    // Fresh relation window per engine run: stale prices and minute anchors
    // from a previous run must not leak into the first report.
    m_relationStates.fill(RelationState{});
    m_xauDeltas.clear();
    m_usidxDeltas.clear();
    m_xbrusdDeltas.clear();
    m_relationTimer.start();
    updateEngineButtons(true);
    appendLog(QStringLiteral("[引擎] 启动轮询：XAUUSD（策略）、USIDX 与 XBRUSD"
                             "（参考价格面板）分别在独立线程中每 1 秒读取。"));
    appendLog(QStringLiteral("[参考价格] 三个面板分别逐条追加 \"XAUUSD:<价格>\"、"
                             "\"USIDX:<价格>\" 与 \"XBRUSD:<价格>\"；"
                             "休市时停止追加并降频至 60 秒。"));
    appendLog(QStringLiteral("[关联] 每分钟在日志中输出 XAU/USIDX/XBRUSD 的分钟涨跌"
                             "及与 XAU 的相关性（近 30 分钟滚动）。"));

    // Automatic real-stop management starts with this engine run: reset the
    // write throttles/guards so the first decision is immediate, but KEEP the
    // peak-PnL trail of positions that are still open — an engine restart
    // must not forget how much profit the position has already given back.
    for (auto &entry : m_autoStopStates)
    {
        entry.second.lastAttemptMs = 0;
        entry.second.lastSentSl = 0.0;
        entry.second.lastSentMs = 0;
        entry.second.lastWarningMs = 0;
        entry.second.lastPeakCloseMs = 0;
    }
    if (false == ui->check_goldTrader->isChecked())
    {
        appendLog(QStringLiteral("[黄金交易员] 未启动：只展示订单数据，不自动调整止损。"
                                 "点击“启动黄金交易员”后才会执行。"));
    }
    else if (ui->check_autoFollow->isChecked())
    {
        if (m_tradfiWritable)
        {
            const double givebackU = ui->spin_peakGiveback->value();
            appendLog(QStringLiteral(
                          "[自动止损] 已启用：浮动亏损→8U 止损；盈利>+2U→"
                          "保本损（开仓价±0.2）；每秒记录最大浮盈，"
                          "最大浮盈 − %1U 作为候选止损，与 8U/保本/1m 超级趋势"
                          "比较取最优；回撤达到 %2U 立即市价平仓；"
                          "超级趋势价再往亏损方向留 1U 缓冲（避免插针），"
                          "仍更优时改用它；只收紧不放宽。")
                          .arg(givebackU, 0, 'f', 2)
                          .arg(givebackU, 0, 'f', 2));
        }
        else
        {
            appendLog(QStringLiteral("[自动止损] 未启用：当前 Key 对 CFD 仅有只读权限，"
                                     "无法修改真实持仓止损。"));
        }
    }
    else
    {
        appendLog(QStringLiteral("[黄金交易员] 已启动，但“自动止损”未勾选："
                                 "只展示订单数据，不调整止损。"));
    }

    // The REST calls block for up to ~10 s on network failures — never run
    // them on the UI thread. The poll threads post results back via a
    // QPointer-guarded queued invocation; closing the window mid-poll is
    // safe (callbacks become no-ops).
    const pulse::ExchangeConfig cfg =
        pulse::ui::VerifyWorker::makeExchangeConfig(m_apiKey, m_apiSecret);
    // Share the parameter block by pointer: the UI thread hot-updates the
    // atomic fields, the engine reads them every round. The shared_ptr keeps
    // the block alive even if the window is destroyed mid-poll.
    const std::shared_ptr<pulse::ui::GoldStrategyEngine::Params> params = m_strategyParams;
    QPointer<SimGateSystem> self(this);
    m_pollThread = std::thread([self, cfg, params]() {
        // Live CFD balance refresh throttle (poll-thread local).
        std::int64_t lastBalanceTs = 0;
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        // The engine's real strategy (EmaResonanceScalper) drives the paper
        // trades; the adapter aggregates ticks into 1m candles and calls the
        // strategy's pure evaluateEntry() on every poll.
        pulse::ui::EngineStrategyAdapter adapter;
        const int warmupCandles = adapter.warmup(cfd);
        // The multi-timeframe EMA panel that used to render here was removed
        // from the window on 2026-09-23 (it had been parked since 2026-09-10:
        // a 7-request kline backfill on this thread bought a display nobody
        // read). The panel slot now carries the USD index reference price.
        // MultiTimeframeEma itself is untouched and still unit-tested.
        QMetaObject::invokeMethod(
            self,
            [self, warmupCandles]() {
                if (self)
                {
                    self->appendLog(warmupCandles > 0
                                        ? QStringLiteral("[策略] K线历史回填 %1 根，预热完成。")
                                              .arg(warmupCandles)
                                        : QStringLiteral("[策略] K线回填失败，将依赖实时轮询累积预热。"));
                }
            },
            Qt::QueuedConnection);

        std::chrono::steady_clock::time_point lastErrorReport{};
        bool firstErrorLogged = false;
        // Market-open tracking: the TradFi ticker carries a "status" field
        // ("open"/"closed"). While closed, evaluation is paused (no new
        // ticks reach the strategy — a closed market can never produce a
        // valid entry) and the poll slows to one request per minute.
        bool marketOpen = true;
        bool lastReportedOpen = true;
        // Market-open protection: entries stay blocked for the first
        // `openProtectMin` minutes after a closed→open transition — opening
        // prices/spreads are unreliable right after the open (gaps, spikes,
        // thin liquidity). Re-arm the timer on every transition.
        std::int64_t marketOpenSinceMs = -1;  // -1 = not armed (already open at start).

        while (true)
        {
            if (nullptr == self || self->m_stopRequested.load(std::memory_order_acquire))
            {
                break;
            }
            // Round start — the pacing at the tail of the loop sleeps only
            // what is left of the interval after the requests above.
            const auto roundStart = std::chrono::steady_clock::now();

            const auto ticker = cfd.getCfdTicker("XAUUSD");
            if (pulse::ok(ticker))
            {
                const nlohmann::json *obj = pulse::ui::VerifyWorker::unwrapData(
                    pulse::ui::VerifyWorker::firstObject(pulse::value(ticker)));
                if (nullptr != obj && obj->contains("last_price"))
                {
                    const double price = pulse::ui::VerifyWorker::jsonToDouble(
                        (*obj)["last_price"]);
                    // The status field is authoritative; absent → assume open.
                    // The tested helper, NOT obj->value("status", "open"):
                    // value() throws type_error.302 when the field is not a
                    // string, and an exception escaping this loop terminates
                    // the process (the helper takes the raw response and
                    // re-unwraps it — three pointer checks, worth the safety).
                    marketOpen =
                        pulse::ui::VerifyWorker::tickerMarketOpen(pulse::value(ticker));

                    if (marketOpen != lastReportedOpen)
                    {
                        // Report the market-state transition exactly once and
                        // (re)arm the open-protection window.
                        lastReportedOpen = marketOpen;
                        if (marketOpen)
                        {
                            marketOpenSinceMs =
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
                        }
                        else
                        {
                            marketOpenSinceMs = -1;  // Re-arm on the next open.
                        }
                        const QString stateText =
                            marketOpen
                                ? QStringLiteral("[行情] XAUUSD 开市，恢复 1 秒轮询与策略评估"
                                                 "（开市保护 %1 分钟，期间不开仓）。")
                                      .arg(params->openProtectMin.load())
                                : QStringLiteral("[行情] XAUUSD 休市（最后价格 %1 USD），"
                                                 "暂停策略评估，轮询降至 60 秒。")
                                      .arg(price, 0, 'f', 2);
                        QMetaObject::invokeMethod(
                            self,
                            [self, stateText]() {
                                if (self)
                                {
                                    self->appendLog(stateText);
                                }
                            },
                            Qt::QueuedConnection);
                    }

                    if (marketOpen && price > 0.0)
                    {
                        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();

                        // Live CFD account balance — throttled to once per
                        // 10 s (an authenticated GET every second invites
                        // rate limiting; 10 s is plenty for a balance row).
                        if (nowMs - lastBalanceTs >= 10'000)
                        {
                            lastBalanceTs = nowMs;
                            const auto assets = cfd.getCfdAssets();
                            if (pulse::ok(assets))
                            {
                                const nlohmann::json *obj =
                                    pulse::ui::VerifyWorker::unwrapData(
                                        pulse::ui::VerifyWorker::firstObject(
                                            pulse::value(assets)));
                                if (nullptr != obj)
                                {
                                    const double equity = safeField(*obj, "equity");
                                    const double balance = safeField(*obj, "balance");
                                    QMetaObject::invokeMethod(
                                        self,
                                        [self, equity, balance]() {
                                            if (self)
                                            {
                                                self->refreshRealCfdBalance(equity, balance);
                                            }
                                        },
                                        Qt::QueuedConnection);
                                }
                            }
                        }

                        // Hot-read the tunable envelope parameters each round
                        // (atomic — the parameter UI writes them).
                        const double tpPoints = params->tpPoints.load();
                        const double slPoints = params->slPoints.load();
                        pulse::ui::TickOutcome outcome =
                            adapter.onTick(price, nowMs, tpPoints, slPoints,
                                           params->cooldownMs.load());
                        // Open-protection gate: evaluation keeps running (the
                        // kline stream stays continuous) but any entry signal
                        // inside the window is downgraded to "no open" — the
                        // first minutes of a fresh market carry unreliable
                        // prices (gaps/spikes), exactly the data the user
                        // does not want to trade on.
                        const std::int64_t protectMs =
                            static_cast<std::int64_t>(params->openProtectMin.load()) * 60'000;
                        if (outcome.decision.has_value() && outcome.decision->shouldOpen
                            && marketOpenSinceMs > 0
                            && nowMs - marketOpenSinceMs < protectMs)
                        {
                            outcome.decision->shouldOpen = false;
                            outcome.decision->reason =
                                QStringLiteral("开市保护期（剩余 %1 秒，开市初期数据不可靠）")
                                    .arg((protectMs - (nowMs - marketOpenSinceMs)) / 1000);
                        }
                        // Trailing stops are applied on the UI thread with the
                        // live price and the hot-tunable point distances.
                        QMetaObject::invokeMethod(
                            self,
                            [self, price, outcome]() {
                                if (self)
                                {
                                    self->onPollResult(price, outcome);
                                }
                            },
                            Qt::QueuedConnection);
                        lastErrorReport = std::chrono::steady_clock::time_point{};
                        firstErrorLogged = false;
                    }
                }
            }
            else
            {
                // Report the first failure immediately, then throttled to
                // one line per kErrorReportInterval.
                const auto now = std::chrono::steady_clock::now();
                if (false == firstErrorLogged
                    || now - lastErrorReport >= kErrorReportInterval)
                {
                    lastErrorReport = now;
                    firstErrorLogged = true;
                    const QString message = QString::fromStdString(
                        pulse::error(ticker).message);
                    QMetaObject::invokeMethod(
                        self,
                        [self, message]() {
                            if (self)
                            {
                                self->appendLog(
                                    QStringLiteral("[行情] 读取失败：%1").arg(message));
                            }
                        },
                        Qt::QueuedConnection);
                }
            }

            // NOTE (2026-09-23): the USD index (USIDX) fetch used to share
            // this loop as a second request per round. Both symbols cost the
            // same REST latency, so serialising them here halved the gold
            // refresh rate — and gold is the trading input. USIDX now runs
            // on its own thread; this loop carries the gold ticker only.

            // Pacing: one round per interval (1 s open, 60 s closed). The
            // REST round trip through the proxy costs ~1-2 s on its own
            // (measured 2026-09-10: XAUUSD ticker 1.2-2.2 s), so sleep only
            // the REMAINDER of the interval — a fixed 1 s sleep would put the
            // effective cadence at latency + 1 s. Interruptible: the stop
            // flag and a destroyed window are checked every 100 ms; the close
            // state is re-checked on every wake-up.
            const std::int64_t intervalMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(kPollInterval).count();
            const std::int64_t elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - roundStart)
                    .count();
            std::int64_t remainingMs = (marketOpen ? intervalMs : 60'000) - elapsedMs;
            if (remainingMs < 0)
            {
                remainingMs = 0;
            }
            const int sleepChunks = static_cast<int>((remainingMs + 99) / 100);
            for (int i = 0; i < sleepChunks; ++i)
            {
                if (nullptr == self
                    || self->m_stopRequested.load(std::memory_order_acquire))
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        QMetaObject::invokeMethod(
            self, [self]() {
                if (self)
                {
                    self->onPollingStopped();
                }
            },
            Qt::QueuedConnection);
    });

    // -----------------------------------------------------------------------
    // Reference-price poll threads (USIDX, XBRUSD) — display-only, decoupled
    // from the gold thread.
    //
    // Why one thread per symbol: each REST round trip costs ~1.7 s, so
    // fetching several symbols back-to-back in ONE loop multiplies the round
    // length and halves each symbol's refresh rate. Independent threads give
    // each symbol its own cadence with no blocking between them.
    //
    // Why this split is safe: the threads share NO mutable state. Neither
    // touches the strategy params, the adapter or the position stores — each
    // posts a price to the panel and nothing else. The single shared object
    // is the m_stopRequested atomic, which every thread only reads.
    // GateRestClient documents "a single instance is NOT thread-safe" but
    // "multiple instances can coexist", so each thread owns its own client
    // (curl_global_init is std::call_once-guarded, process-wide).
    //
    // Cost the operator accepted: one ~1.7 s cadence per symbol puts ~1.8
    // requests/s on the REST API with three poll threads.
    // -----------------------------------------------------------------------
    auto startReferencePoll = [self, cfg](const std::string &symbol,
                                          const QString &displayName) {
        return std::thread([self, cfg, symbol, displayName]() {
            pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
            std::chrono::steady_clock::time_point lastErrorReport{};
            bool firstErrorLogged = false;
            // Each symbol keeps its OWN session — it must not inherit gold's
            // market state. Closed → one request per minute, exactly the
            // cadence this fetch had while it shared the gold loop.
            bool marketOpen = true;
            bool lastReportedOpen = true;
            while (true)
            {
                if (nullptr == self
                    || self->m_stopRequested.load(std::memory_order_acquire))
                {
                    break;
                }
                const auto roundStart = std::chrono::steady_clock::now();

                const auto ticker = cfd.getCfdTicker(symbol);
                if (pulse::ok(ticker))
                {
                    marketOpen =
                        pulse::ui::VerifyWorker::tickerMarketOpen(pulse::value(ticker));
                    const std::optional<double> price =
                        pulse::ui::VerifyWorker::tickerLastPrice(pulse::value(ticker));

                    // Report every session transition exactly once. Without
                    // this a closed session would simply stop appending,
                    // leaving the panel looking frozen for the same reason
                    // the replaced-text version did — the thing this tape
                    // exists to rule out.
                    if (marketOpen != lastReportedOpen)
                    {
                        lastReportedOpen = marketOpen;
                        const QString stateText =
                            marketOpen
                                ? QStringLiteral("[行情] %1 开市，恢复逐条追加。")
                                      .arg(displayName)
                                : QStringLiteral("[行情] %1 休市，暂停追加，"
                                                 "轮询降至 60 秒。")
                                      .arg(displayName);
                        QMetaObject::invokeMethod(
                            self,
                            [self, stateText]() {
                                if (self)
                                {
                                    self->appendLog(stateText);
                                }
                            },
                            Qt::QueuedConnection);
                    }

                    // A closed session quotes 0, which tickerLastPrice
                    // rejects — so no line is appended while closed.
                    if (price.has_value())
                    {
                        lastErrorReport = std::chrono::steady_clock::time_point{};
                        firstErrorLogged = false;
                        const double value = *price;
                        const QString symbolText = QString::fromStdString(symbol);
                        QMetaObject::invokeMethod(
                            self,
                            [self, symbolText, value]() {
                                if (self)
                                {
                                    self->appendReferencePrice(symbolText, value);
                                }
                            },
                            Qt::QueuedConnection);
                    }
                }
                else
                {
                    // First failure immediate, then one line per interval —
                    // the same throttle shape as the gold loop.
                    const auto now = std::chrono::steady_clock::now();
                    if (false == firstErrorLogged
                        || now - lastErrorReport >= kErrorReportInterval)
                    {
                        lastErrorReport = now;
                        firstErrorLogged = true;
                        const QString message = QString::fromStdString(
                            pulse::error(ticker).message);
                        QMetaObject::invokeMethod(
                            self,
                            [self, displayName, message]() {
                                if (self)
                                {
                                    self->appendLog(
                                        QStringLiteral("[行情] %1 读取失败：%2")
                                            .arg(displayName, message));
                                }
                            },
                            Qt::QueuedConnection);
                    }
                }

                // Same pacing rule as the gold loop: sleep only the REMAINDER
                // of the interval, so the cadence is the REST latency rather
                // than latency + a fixed sleep. Interruptible every 100 ms.
                const std::int64_t intervalMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(kPollInterval)
                        .count();
                const std::int64_t elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - roundStart)
                        .count();
                std::int64_t remainingMs = (marketOpen ? intervalMs : 60'000) - elapsedMs;
                if (remainingMs < 0)
                {
                    remainingMs = 0;
                }
                const int sleepChunks = static_cast<int>((remainingMs + 99) / 100);
                for (int i = 0; i < sleepChunks; ++i)
                {
                    if (nullptr == self
                        || self->m_stopRequested.load(std::memory_order_acquire))
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    };

    m_usidxThread =
        startReferencePoll(std::string("USIDX"), QStringLiteral("美元指数 USIDX"));
    m_xbrusdThread = startReferencePoll(std::string("XBRUSD"), QStringLiteral("XBRUSD"));
}

void SimGateSystem::stopEnginePolling()
{
    // Stop the relation clock first — no tick may append to the log while
    // the poll threads are being torn down.
    m_relationTimer.stop();

    // Signal first: ALL polling threads test this flag, so raise it before
    // any join can block. startEnginePolling() clears it again, so setting it
    // while idle is harmless.
    m_stopRequested.store(true, std::memory_order_release);

    // Join unconditionally, NOT behind the `m_polling` guard below:
    // onPollingStopped() — posted by the gold thread on its way out — may
    // already have cleared the flag, and a std::thread left joinable at
    // destruction calls std::terminate. A dead thread joins instantly, so
    // the extra calls cost nothing.
    if (m_pollThread.joinable())
    {
        m_pollThread.join();  // Returns within ~100 ms (interruptible sleep).
    }
    if (m_usidxThread.joinable())
    {
        m_usidxThread.join();  // Same interruptible-sleep bound.
    }
    if (m_xbrusdThread.joinable())
    {
        m_xbrusdThread.join();  // Same interruptible-sleep bound.
    }

    if (false == m_polling)
    {
        return;  // Nothing was running — the threads above were already dead.
    }
    m_polling = false;
    updateEngineButtons(false);
    appendLog(QStringLiteral("[引擎] 轮询已停止。"));
}

// ---------------------------------------------------------------------------
// 关联分析 — once per minute while polling.
//
// Direction: each symbol's latest price minus its price at the previous
// minute tick (the anchor). "涨/跌" at half the exchange's quote tick,
// otherwise "平"; a symbol that has not printed for two minutes is
// "无数据" (its own market may be closed while another's is open).
//
// Tightness: Pearson r between XAU's per-minute changes and each of
// USIDX/XBRUSD over the last 30 aligned minutes. |r| picks the closer
// relationship; the sign says same-direction (r > 0) or opposite (r < 0) —
// e.g. gold up + dollar index down is a strong NEGATIVE r, which is just as
// "tight" as a positive one. Only minutes where all three symbols produced
// a fresh delta enter the window, so the series stay aligned.
// ---------------------------------------------------------------------------
void SimGateSystem::reportMarketRelations()
{
    if (false == m_polling)
    {
        return;  // A late timeout must not print after "[引擎] 轮询已停止。"
    }

    // A quote older than this counts as "no data": the three symbols have
    // independent sessions, and a closed market simply stops updating.
    constexpr std::int64_t kStaleMs = 120'000;
    // Rolling window and the minimum number of aligned samples before r is
    // reported (a single minute's move is noise, not a correlation).
    constexpr std::size_t kMaxSamples = 30;
    constexpr std::size_t kMinSamples = 10;

    static const char *const kLabels[kRelationCount] = { "XAU", "USIDX", "XBRUSD" };
    // Half a quote tick: at the exchange's display precision a smaller move
    // is flat. XAUUSD/XBRUSD quote 2 decimals, USIDX 3.
    static const double kEps[kRelationCount] = { 0.005, 0.0005, 0.005 };

    const std::int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
    std::array<double, kRelationCount> deltas{};
    bool haveAllDeltas = true;
    QStringList parts;

    for (int i = 0; i < kRelationCount; ++i)
    {
        RelationState &state = m_relationStates[i];
        const bool fresh = state.hasPrice && (nowMs - state.lastUpdateMs <= kStaleMs);
        const bool haveDelta = fresh && state.hasMinuteAnchor;
        if (haveDelta)
        {
            deltas[i] = state.lastPrice - state.minuteAnchor;
        }
        else
        {
            haveAllDeltas = false;
        }

        QString direction;
        if (false == fresh)
        {
            direction = QStringLiteral("无数据");
        }
        else if (false == haveDelta)
        {
            direction = QStringLiteral("待定");
        }
        else if (deltas[i] > kEps[i])
        {
            direction = QStringLiteral("涨");
        }
        else if (deltas[i] < -kEps[i])
        {
            direction = QStringLiteral("跌");
        }
        else
        {
            direction = QStringLiteral("平");
        }
        parts << QStringLiteral("%1:%2").arg(QString::fromLatin1(kLabels[i]), direction);

        // Roll the anchor for the next minute. After a stale gap the next
        // incoming quote re-arms it (appendReferencePrice), so the report
        // resumes cleanly when that market reopens.
        if (fresh)
        {
            state.minuteAnchor = state.lastPrice;
            state.hasMinuteAnchor = true;
        }
        else
        {
            state.hasMinuteAnchor = false;
        }
    }

    // Correlate only complete minutes — all three deltas must cover the same
    // interval to keep the series aligned.
    if (haveAllDeltas)
    {
        m_xauDeltas.push_back(deltas[kRelationXau]);
        m_usidxDeltas.push_back(deltas[kRelationUsidx]);
        m_xbrusdDeltas.push_back(deltas[kRelationXbrusd]);
        while (m_xauDeltas.size() > kMaxSamples)
        {
            m_xauDeltas.pop_front();
            m_usidxDeltas.pop_front();
            m_xbrusdDeltas.pop_front();
        }
    }

    const QString head = QStringLiteral("[关联] %1 %2")
                             .arg(beijingNow(), parts.join(QStringLiteral(" ")));

    if (m_xauDeltas.size() < kMinSamples)
    {
        appendLog(QStringLiteral("%1 → 相关性样本积累中（%2/%3 分钟）")
                      .arg(head)
                      .arg(m_xauDeltas.size())
                      .arg(kMinSamples));
        return;
    }

    const std::optional<double> rUsidx =
        pearsonCorrelation(m_xauDeltas, m_usidxDeltas);
    const std::optional<double> rXbrusd =
        pearsonCorrelation(m_xauDeltas, m_xbrusdDeltas);

    // Compare |r|: the sign only says same/opposite, the magnitude is the
    // tightness the operator asked for. A zero-variance series (a flat
    // index over the whole window) has no defined r and is left out.
    bool haveClosest = false;
    QString closestName;
    double closestR = 0.0;
    QStringList relations;
    const auto addCandidate = [&](const QString &name, double r) {
        relations << QStringLiteral("%1（%2 r=%3%4）")
                         .arg(name)
                         .arg(r >= 0.0 ? QStringLiteral("同向")
                                       : QStringLiteral("反向"))
                         .arg(r >= 0.0 ? QStringLiteral("+") : QString())
                         .arg(r, 0, 'f', 2);
        if (false == haveClosest || std::abs(r) > std::abs(closestR))
        {
            haveClosest = true;
            closestName = name;
            closestR = r;
        }
    };
    if (rUsidx.has_value())
    {
        addCandidate(QStringLiteral("USIDX"), *rUsidx);
    }
    if (rXbrusd.has_value())
    {
        addCandidate(QStringLiteral("XBRUSD"), *rXbrusd);
    }

    if (false == haveClosest)
    {
        appendLog(QStringLiteral("%1 → 近 %2 分钟 XAU、USIDX、XBRUSD 均无明显波动，"
                                 "相关性不可用")
                      .arg(head)
                      .arg(m_xauDeltas.size()));
        return;
    }

    appendLog(QStringLiteral("%1 → 与 XAU 相关性（近 %2 分钟）：%3；最紧密：%4")
                  .arg(head)
                  .arg(m_xauDeltas.size())
                  .arg(relations.join(QStringLiteral("；")), closestName));
}

void SimGateSystem::onPollResult(double price, const pulse::ui::TickOutcome &outcome)
{
    if (false == m_polling)
    {
        return;
    }
    m_lastPrice = price;
    // The gold price goes to its own tape pane; the log pane carries only
    // events and state transitions (it used to repeat this line every poll).
    appendReferencePrice(QStringLiteral("XAUUSD"), price);
    handleDecision(outcome.decision);
    showTrendReadout(outcome);
    settleSimPositions(price);
    // Trailing stops run AFTER settlement: a TP/SL hit closes first, then
    // the surviving positions get their stops moved up.
    applyTrailingStops(price);
    // Real-account stops: 8U loss / break-even / 1m SuperTrend (see
    // autoManageRealStops). Only acts while the engine polls, i.e. right
    // after 启动引擎.
    autoManageRealStops(price, outcome);
    // Live equity readout: the simulated balance and every open position
    // reflect the floating PnL at the current price.
    refreshSimBalance(price);
    refreshSimPositions(price);
}

void SimGateSystem::showTrendReadout(const pulse::ui::TickOutcome &outcome)
{
    // Print the four-timeframe SuperTrend line only when a direction
    // actually changed — keeps the pane readable during long quiet runs.
    if (false == outcome.trendChanged)
    {
        return;
    }
    QStringList parts;
    for (const auto &trend : outcome.trends)
    {
        if (false == trend.valid)
        {
            parts << QStringLiteral("%1: 预热中").arg(trend.label);
            continue;
        }
        parts << QStringLiteral("%1: %2 ST=%3")
                     .arg(trend.label,
                          trend.bullish ? QStringLiteral("开多") : QStringLiteral("开空"))
                     .arg(trend.supertrend, 0, 'f', 2);
    }
    appendLog(QStringLiteral("[趋势] %1").arg(parts.join(QStringLiteral(" | "))));
}

void SimGateSystem::applyTrailingStops(double price)
{
    const double tpPoints = m_strategyParams->tpPoints.load();
    const double slPoints = m_strategyParams->slPoints.load();
    const SimTradeEngine::StopsUpdateResult update =
        SimTradeEngine::updateStops(m_apiKey, price, tpPoints, slPoints);
    if (update.updatedCount > 0)
    {
        refreshSimPositions(price);
        appendLog(QStringLiteral("[策略] 移动止盈止损：%1 个持仓按当前价 %2 更新（TP/SL ±%3/%4 点）")
                      .arg(update.updatedCount)
                      .arg(price, 0, 'f', 2)
                      .arg(tpPoints, 0, 'f', 1)
                      .arg(slPoints, 0, 'f', 1));
    }
}

void SimGateSystem::handleDecision(const std::optional<pulse::ui::AdapterDecision> &decision)
{
    if (false == decision.has_value())
    {
        return;  // No entry candidate this round.
    }
    if (false == decision->shouldOpen)
    {
        appendTrade(QStringLiteral("[策略] 不开仓：%1").arg(decision->reason));
        return;
    }

    // Volume and leverage come from the hot-tunable parameters (atomic).
    const double volumeLots = m_strategyParams->openVolumeLots.load();
    const double leverage = m_strategyParams->openLeverage.load();
    const bool isLong = (decision->side != QStringLiteral("short"));
    appendTrade(QStringLiteral("[策略] 开仓%1 %2 手 @%3 | 止盈 %4 | 止损 %5 | %6")
                    .arg(isLong ? QStringLiteral("买多") : QStringLiteral("卖空"))
                    .arg(volumeLots, 0, 'f', 2)
                    .arg(decision->entryPrice, 0, 'f', 2)
                    .arg(decision->takeProfit, 0, 'f', 2)
                    .arg(decision->stopLoss, 0, 'f', 2)
                    .arg(decision->reason));

    // Paper trade with the real CFD model: margin = notional / leverage,
    // commission on the BUY side (shorts are free). No real order is placed.
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const SimTradeEngine::OpenResult open =
        isLong ? SimTradeEngine::openLong(m_apiKey, volumeLots,
                                          decision->entryPrice,
                                          decision->takeProfit,
                                          decision->stopLoss, leverage, nowMs)
               : SimTradeEngine::openShort(m_apiKey, volumeLots,
                                           decision->entryPrice,
                                           decision->takeProfit,
                                           decision->stopLoss, leverage, nowMs);
    if (open.ok)
    {
        refreshSimBalance(decision->entryPrice);
        refreshSimPositions(decision->entryPrice);
        appendLog(QStringLiteral("[模拟] 开仓%1 %2 手 @%3：保证金 %4 USDT + 手续费 %5 USDT，"
                                 "可用余额 %6 USDT（杠杆 %7x）")
                      .arg(isLong ? QStringLiteral("买多") : QStringLiteral("卖空"))
                      .arg(volumeLots, 0, 'f', 2)
                      .arg(decision->entryPrice, 0, 'f', 2)
                      .arg(open.marginUsdt, 0, 'f', 2)
                      .arg(open.feeUsdt, 0, 'f', 2)
                      .arg(SimAccountStore::load(m_apiKey)->balanceUsdt, 0, 'f', 2)
                      .arg(leverage, 0, 'f', 0));
    }
    else
    {
        appendTrade(QStringLiteral("[模拟] 开仓失败：%1").arg(open.error));
    }
}

void SimGateSystem::settleSimPositions(double price)
{
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto closes = SimTradeEngine::checkAndClose(m_apiKey, price, nowMs);
    for (const auto &close : closes)
    {
        const QString pnlText = QStringLiteral("%1 USD")
                                    .arg(close.realizedPnl, 0, 'f', 2);
        appendTrade(QStringLiteral("[模拟] 平仓 %1（%2）@%3：盈亏 %4，"
                                   "释放保证金 %5 USDT")
                        .arg(close.positionId, close.reason)
                        .arg(close.exitPrice, 0, 'f', 2)
                        .arg(close.realizedPnl >= 0.0 ? QStringLiteral("+") + pnlText
                                                      : pnlText)
                        .arg(close.releasedMargin, 0, 'f', 2));
        appendLog(QStringLiteral("[模拟] 持仓 %1 触发%2 平仓，结算后可用余额 %3 USDT")
                      .arg(close.positionId, close.reason)
                      .arg(SimAccountStore::load(m_apiKey)->balanceUsdt, 0, 'f', 2));
    }
    if (false == closes.empty())
    {
        refreshSimBalance(price);
        refreshSimPositions(price);
    }
}

void SimGateSystem::refreshSimPositions(double price)
{
    const auto positions = SimTradeEngine::positions(m_apiKey);
    if (positions.empty())
    {
        ui->label_simPositions->setText(QStringLiteral("无"));
        return;
    }
    QStringList parts;
    for (const auto &p : positions)
    {
        const bool isLong = (p.side != QStringLiteral("short"));
        const double pnl = floatingPnl(p, price);
        parts << QStringLiteral("%1手@%2 %3 TP%4 SL%5 保证金%6 浮盈%7%8")
                     .arg(p.volumeLots, 0, 'f', 2)
                     .arg(p.entryPrice, 0, 'f', 2)
                     .arg(isLong ? QStringLiteral("多") : QStringLiteral("空"))
                     .arg(p.takeProfit, 0, 'f', 2)
                     .arg(p.stopLoss, 0, 'f', 2)
                     .arg(p.marginUsdt, 0, 'f', 2)
                     .arg(pnl >= 0.0 ? QStringLiteral("+") : QString())
                     .arg(pnl, 0, 'f', 2);
    }
    ui->label_simPositions->setText(parts.join(QStringLiteral("  |  ")));
}

void SimGateSystem::onPollingStopped()
{
    m_polling = false;
    updateEngineButtons(false);
}

void SimGateSystem::updateEngineButtons(bool running)
{
    ui->pushBtn_startEngine->setEnabled(false == running);
    ui->pushBtn_stopEngine->setEnabled(running);
}

void SimGateSystem::appendLog(const QString &text)
{
    // appendPlainText, not QTextEdit::append: the log carries strategy text
    // that could look like markup ("<" in a comparison, for instance), which
    // QTextEdit::append would render as HTML. The pane keeps the latest 5000
    // lines (maximumBlockCount, set in MainWindow.ui).
    ui->textEdit_log->appendPlainText(text);
}

void SimGateSystem::appendTrade(const QString &text)
{
    ui->textEdit_trade->append(text);
}
