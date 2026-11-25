// main_window.cpp — the constructor only runs ui->setupUi(this) plus the
// balance texts into label_balance / label_simBalance. The 模拟账户 flow and
// the engine polling (启动引擎 = three 1-second poll threads: XAUUSD → the
// strategy decision, and XAUUSD/USIDX/XBRUSD → their own price tape panes)
// are the active logic; all widgets come from MainWindow.ui.

#include "MainWindow.hpp"

#include "EngineStrategyAdapter.hpp"
#include "ExternalSignalStore.hpp"
#include "SimAccountStore.hpp"
#include "SimOrderAnalyzer.hpp"
#include "SimOrderDb.hpp"
#include "SimTradeEngine.hpp"
#include "TradeHistoryDialog.hpp"
#include "LlmOverlayDialog.hpp"

#include "exchange/GateRestClient.hpp"

#include "ui_MainWindow.h"

#include <QColor>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
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

// External 5-minute analysis policy: the user fixed the paper size and
// leverage; the analysis supplies the prices. Risk is capped at 50 USDT as
// an abnormal-signal guard (the 100 USDT account deliberately does not use a
// 1% gate — normal 7.5–30 USDT stops must pass).
constexpr double kExternalLots = 0.01;
constexpr double kExternalLeverage = 1000.0;
constexpr double kExternalMaxRiskUsd = 50.0;
constexpr double kExternalMaxDriftCapUsd = 20.0;

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

    // External 5-minute analysis mode: the built-in strategies and automatic
    // real-position stops are disabled permanently. The old strategy
    // parameter panel (params, 黄金交易员, 自动止损, 动态止盈止损) is hidden —
    // its controls remain in the .ui only because legacy slots still
    // reference them; nothing in it can act. Manual real-position
    // close/TP-SL edits stay available.
    ui->groupBox_params->hide();
    ui->check_goldTrader->setChecked(false);
    ui->check_autoFollow->setChecked(false);

    // 真实持仓 table: the symbol column takes the spare width, the numeric
    // columns size to their headers. Row content is filled by
    // onRealPositionsUpdated() from the background position refresh.
    QHeaderView *positionsHeader = ui->table_realPositions->horizontalHeader();
    positionsHeader->setMinimumSectionSize(40);
    const int realWidths[] = { 100, 70, 80, 90, 90, 90, 90, 110, 70 };
    for (int column = 0; column < ui->table_realPositions->columnCount()
         && column < static_cast<int>(sizeof(realWidths) / sizeof(realWidths[0])); ++column)
    {
        positionsHeader->setSectionResizeMode(column, QHeaderView::Interactive);
        positionsHeader->resizeSection(column, realWidths[column]);
    }
    // A little taller than the text-only rows so the 平仓 button fits.
    ui->table_realPositions->verticalHeader()->setDefaultSectionSize(28);
    // Only the TP/SL cells keep Qt::ItemIsEditable (set per item below), and
    // a committed edit is synced to the real position.
    ui->table_realPositions->setEditTriggers(QAbstractItemView::DoubleClicked
                                             | QAbstractItemView::EditKeyPressed);
    connect(ui->table_realPositions, &QTableWidget::itemChanged, this,
            &SimGateSystem::onRealPositionItemChanged);

    // 模拟持仓 table: same columns/layout as 真实持仓. It is read-only except
    // for the per-row 平仓 button (a deliberate manual override; the external
    // 5-minute signal remains the normal exit path).
    QHeaderView *simHeader = ui->table_simPositions->horizontalHeader();
    simHeader->setMinimumSectionSize(40);
    const int simWidths[] = { 100, 70, 80, 90, 90, 90, 90, 100, 110, 70 };
    for (int column = 0; column < ui->table_simPositions->columnCount()
         && column < static_cast<int>(sizeof(simWidths) / sizeof(simWidths[0])); ++column)
    {
        simHeader->setSectionResizeMode(column, QHeaderView::Interactive);
        simHeader->resizeSection(column, simWidths[column]);
    }
    ui->table_simPositions->verticalHeader()->setDefaultSectionSize(28);

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
        loadClosedTradeHistory();
    }
    else
    {
        appendLog(QStringLiteral("[模拟] 订单数据库初始化失败（SQLite 驱动或文件不可用），"
                                 "订单历史将不落库"));
    }

    appendLog(QStringLiteral("[引擎] 就绪：点击“启动引擎”开始轮询 XAUUSD/USIDX/XBRUSD；"
                             "交易由外部 5 分钟分析信号驱动。"));

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
    // External 5-minute analysis signal inbox. The timer runs only while the
    // engine is polling; it reads external_signal.json on the UI thread and
    // writes ack/state/journal next to sim_account.json.
    m_externalSignalTimer.setInterval(1'000);
    connect(&m_externalSignalTimer, &QTimer::timeout, this,
            &SimGateSystem::onExternalSignalTimer);
    m_recentSignalIds = pulse::ui::ExternalSignalStore::loadRecentSignalIds();
    for (const QString &id : m_recentSignalIds)
    {
        m_processedSignalIds.insert(id);
    }
    if (false == m_recentSignalIds.isEmpty())
    {
        m_lastProcessedSignalId = m_recentSignalIds.back();
    }
    appendLog(QStringLiteral("[模式] 外部信号模式：内置策略/黄金交易员/自动止损已停用；"
                             "固定 0.01 手、1000x、单仓。"));
    appendLog(QStringLiteral("[外部信号] 监听：%1")
                  .arg(pulse::ui::ExternalSignalStore::signalPath()));
    appendLog(QStringLiteral("[外部信号] 回执：%1")
                  .arg(pulse::ui::ExternalSignalStore::ackPath()));
    appendLog(QStringLiteral("[外部信号] 状态：%1")
                  .arg(pulse::ui::ExternalSignalStore::statePath()));

    // Claude Code advisory overlay: analysis-only viewer. The 5-minute audit
    // is written here; the isolated LLM盘面 dialog displays it. Nothing is
    // written to the v10 log pane.
    m_llmOverlayTimer.setInterval(5'000);
    connect(&m_llmOverlayTimer, &QTimer::timeout, this,
            &SimGateSystem::pollLlmOverlay);
    m_llmOverlayTimer.start();
    QTimer::singleShot(0, this, &SimGateSystem::pollLlmOverlay);

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

    // First use: initial-balance setup. Later clicks: full reset — the
    // entered balance replaces the account, open positions are dropped and
    // the local order history is cleared.
    const bool isReset = existing.has_value();
    const QString title = isReset ? QStringLiteral("重置模拟账户")
                                  : QStringLiteral("初始余额设置");
    const QString prompt =
        isReset ? QStringLiteral("重置后余额（USDT，将清空持仓与历史交易）：")
                : QStringLiteral("初始余额（USDT）：");
    const double current = isReset ? existing->balanceUsdt : 0.0;

    bool ok = false;
    const double value = QInputDialog::getDouble(
        this, title, prompt, current, 0.0, 9999999999.0, 2, &ok);
    if (false == ok)
    {
        return; // Cancelled — keep the stored account/history untouched.
    }

    pulse::ui::SimAccount account;
    account.balanceUsdt = value;
    account.updatedAt = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    account.positions.clear();
    if (false == SimAccountStore::saveAccount(m_apiKey, account))
    {
        appendLog(QStringLiteral("[模拟账户] 保存失败：%1").arg(SimAccountStore::filePath()));
        return;
    }

    // Clear the persisted order history as part of the reset. A database
    // failure must not undo the balance reset, but it is reported.
    const bool historyCleared = pulse::ui::SimOrderDb::clearAll(m_apiKey);

    // The trade pane held the old history/log lines; clear them so the reset
    // visibly starts from an empty account.
    ui->textEdit_trade->clear();
    refreshSimBalance(0.0);
    refreshSimPositions(0.0);
    writeExternalState(true);
    if (nullptr != m_tradeHistoryDialog)
    {
        m_tradeHistoryDialog->reload();
    }

    appendTrade(QStringLiteral("[模拟账户] 已重置：余额 %1 USDT，持仓与历史交易已清空%2")
                    .arg(QString::number(value, 'f', 2),
                         historyCleared ? QString()
                                        : QStringLiteral("（历史数据库清理失败）")));
}

void SimGateSystem::on_pushBtn_tradeHistory_clicked()
{
    // Non-modal by design: show() (not exec()) so the 1 s price poll and the
    // external-signal executor keep running while the table is open.
    if (nullptr == m_tradeHistoryDialog)
    {
        m_tradeHistoryDialog = new pulse::ui::TradeHistoryDialog(m_apiKey, this);
    }
    // Re-read the store on every click so a restart/reset is reflected.
    m_tradeHistoryDialog->reload();
    m_tradeHistoryDialog->show();
    m_tradeHistoryDialog->raise();
    m_tradeHistoryDialog->activateWindow();
}

void SimGateSystem::on_pushBtn_llmOverlay_clicked()
{
    // The overlay viewer is analysis-only and non-modal. It reads the latest
    // audit record written by pollLlmOverlay(); it cannot place an order.
    if (nullptr == m_llmOverlayDialog)
    {
        m_llmOverlayDialog = new pulse::ui::LlmOverlayDialog(this);
    }
    m_llmOverlayDialog->reload();
    m_llmOverlayDialog->show();
    m_llmOverlayDialog->raise();
    m_llmOverlayDialog->activateWindow();
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
        // External-signal mode: the legacy 黄金交易员 stop manager must never
        // run automatically. The control is also disabled in the constructor;
        // this guard protects against programmatic/state changes.
        const QSignalBlocker blocker(ui->check_goldTrader);
        ui->check_goldTrader->setChecked(false);
        appendLog(QStringLiteral("[黄金交易员] 已停用：外部信号模式下不运行内置策略/自动止损。"));
        return;
    }
    m_autoStopStates.clear();
    appendLog(QStringLiteral("[黄金交易员] 已停止：只展示订单数据，不再自动调整止损。"));
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

void SimGateSystem::startEngine()
{
    on_pushBtn_startEngine_clicked();
}

bool SimGateSystem::xau5mHeartbeatFresh() const
{
    QString heartbeatError;
    const QJsonObject heartbeat =
        pulse::ui::ExternalSignalStore::readHeartbeat(&heartbeatError);
    const QDateTime heartbeatAt = QDateTime::fromString(
        heartbeat.value(QLatin1String("updated_at")).toString(), Qt::ISODate);
    if (false == heartbeatAt.isValid())
    {
        return false;
    }
    const qint64 ageSec = heartbeatAt.secsTo(QDateTime::currentDateTime());
    return ageSec >= 0 && ageSec <= 12 * 60;
}

void SimGateSystem::startEnginePolling()
{
    // xau-5m coupling: the loop writes a heartbeat at the start/end of every
    // round. A missing heartbeat is not a reason to block the price poll: the
    // engine keeps polling and starts consuming external signals as soon as
    // the loop appears. This lets the app be launched first (or automatically)
    // and wait for xau-5m instead of showing a blocking modal.
    {
        QString heartbeatError;
        const QJsonObject heartbeat =
            pulse::ui::ExternalSignalStore::readHeartbeat(&heartbeatError);
        const QDateTime heartbeatAt = QDateTime::fromString(
            heartbeat.value(QLatin1String("updated_at")).toString(), Qt::ISODate);
        const qint64 ageSec = heartbeatAt.isValid()
                                  ? heartbeatAt.secsTo(QDateTime::currentDateTime())
                                  : -1;
        const bool fresh = (ageSec >= 0 && ageSec <= 12 * 60);
        m_xau5mHeartbeatFresh = fresh;
        if (false == fresh)
        {
            appendLog(QStringLiteral(
                "[外部信号] 未检测到 xau-5m 心跳：先持续轮询 XAUUSD/USIDX/XBRUSD，"
                "暂不交易；xau-5m 启动后自动接收信号。"));
            m_lastHeartbeatWarnMs = QDateTime::currentMSecsSinceEpoch();
        }
        else
        {
            appendLog(QStringLiteral("[外部信号] xau-5m 心跳正常（%1 秒前）。").arg(ageSec));
        }
    }

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
    appendLog(QStringLiteral("[引擎] 启动轮询：XAUUSD（行情）、USIDX 与 XBRUSD"
                             "（参考价格面板）分别在独立线程中每 1 秒读取；"
                             "交易只由外部 5 分钟分析信号驱动。"));
    appendLog(QStringLiteral("[参考价格] 三个面板分别逐条追加 \"XAUUSD:<价格>\"、"
                             "\"USIDX:<价格>\" 与 \"XBRUSD:<价格>\"；"
                             "休市时停止追加并降频至 60 秒。"));
    appendLog(QStringLiteral("[关联] 每分钟在日志中输出 XAU/USIDX/XBRUSD 的分钟涨跌"
                             "及与 XAU 的相关性（近 30 分钟滚动）。"));

    m_marketOpen.store(false, std::memory_order_release);
    m_marketStatusKnown.store(false, std::memory_order_release);
    m_waitingPriceSignalId.clear();
    m_externalSignalTimer.start();
    appendLog(QStringLiteral("[外部信号] 已启动 1 秒监听：%1")
                  .arg(pulse::ui::ExternalSignalStore::signalPath()));
    appendLog(QStringLiteral("[外部信号] 仅当引擎运行、行情新鲜且市场开市时执行；"
                             "固定 0.01 手 / 1000x / 单仓。"));
    writeExternalState(true);

    // The REST calls block for up to ~10 s on network failures — never run
    // them on the UI thread. The poll threads post results back via a
    // QPointer-guarded queued invocation; closing the window mid-poll is
    // safe (callbacks become no-ops).
    const pulse::ExchangeConfig cfg =
        pulse::ui::VerifyWorker::makeExchangeConfig(m_apiKey, m_apiSecret);
    QPointer<SimGateSystem> self(this);
    m_pollThread = std::thread([self, cfg]() {
        // Live CFD balance refresh throttle (poll-thread local).
        std::int64_t lastBalanceTs = 0;
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);

        std::chrono::steady_clock::time_point lastErrorReport{};
        bool firstErrorLogged = false;
        // Market-open tracking: the TradFi ticker carries a "status" field
        // ("open"/"closed"). While closed the poll slows to one request per
        // minute and external signals are refused by the UI gate.
        bool marketOpen = true;
        bool lastReportedOpen = true;

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
                    if (self)
                    {
                        self->m_marketOpen.store(marketOpen, std::memory_order_release);
                        self->m_marketStatusKnown.store(true, std::memory_order_release);
                    }

                    if (marketOpen != lastReportedOpen)
                    {
                        // Report the market-state transition exactly once.
                        lastReportedOpen = marketOpen;
                        const QString stateText =
                            marketOpen
                                ? QStringLiteral("[行情] XAUUSD 开市，恢复 1 秒轮询；"
                                                 "等待外部 5 分钟分析信号。")
                                : QStringLiteral("[行情] XAUUSD 休市（最后价格 %1 USD），"
                                                 "暂停外部信号执行，轮询降至 60 秒。")
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

                        // External-signal mode: the poll thread only feeds the
                        // price; the UI thread updates the tape, settles TP/SL,
                        // refreshes the account and processes external JSON
                        // signals. No built-in strategy decision is produced.
                        QMetaObject::invokeMethod(
                            self,
                            [self, price]() {
                                if (self)
                                {
                                    self->onGoldTick(price);
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

        if (self)
        {
            self->m_marketOpen.store(false, std::memory_order_release);
            self->m_marketStatusKnown.store(false, std::memory_order_release);
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
    // Stop the relation clock and the external-signal reader first — no tick
    // or signal may append to the log while the poll threads are torn down.
    m_relationTimer.stop();
    m_externalSignalTimer.stop();
    m_marketOpen.store(false, std::memory_order_release);
    m_marketStatusKnown.store(false, std::memory_order_release);
    m_waitingPriceSignalId.clear();

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
    writeExternalState(true);
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

void SimGateSystem::onGoldTick(double price)
{
    if (false == m_polling)
    {
        return;
    }
    m_lastPrice = price;
    m_lastPriceMs = QDateTime::currentMSecsSinceEpoch();
    // The gold price goes to its own tape pane; the log pane carries only
    // events and state transitions (it used to repeat this line every poll).
    appendReferencePrice(QStringLiteral("XAUUSD"), price);
    // TP/SL settlement is execution, not a strategy: keep it running. External
    // positions have no automatic trailing; their stops come from the signal.
    settleSimPositions(price);
    refreshSimBalance(price);
    refreshSimPositions(price);
    writeExternalState(false);
}

void SimGateSystem::onExternalSignalTimer()
{
    if (false == m_polling)
    {
        return;
    }

    // Periodic xau-5m liveness check: a stale heartbeat while polling means the
    // analysis loop stopped. Warn (throttled) but keep the price poll running.
    const qint64 heartbeatNow = QDateTime::currentMSecsSinceEpoch();
    if (heartbeatNow - m_lastHeartbeatCheckMs >= 30'000)
    {
        m_lastHeartbeatCheckMs = heartbeatNow;
        QString heartbeatError;
        const QJsonObject heartbeat =
            pulse::ui::ExternalSignalStore::readHeartbeat(&heartbeatError);
        const QDateTime heartbeatAt = QDateTime::fromString(
            heartbeat.value(QLatin1String("updated_at")).toString(), Qt::ISODate);
        const qint64 ageSec = heartbeatAt.isValid()
                                  ? heartbeatAt.secsTo(QDateTime::currentDateTime())
                                  : -1;
        const bool fresh = (ageSec >= 0 && ageSec <= 12 * 60);
        if (fresh != m_xau5mHeartbeatFresh)
        {
            m_xau5mHeartbeatFresh = fresh;
            if (fresh)
            {
                appendLog(QStringLiteral(
                    "[外部信号] 检测到 xau-5m 心跳：开始接收 5 分钟分析信号。"));
            }
            else
            {
                appendLog(QStringLiteral(
                    "[外部信号] xau-5m 心跳已过期/未检测到；继续轮询行情，"
                    "等待 xau-5m 恢复。"));
                m_lastHeartbeatWarnMs = heartbeatNow;
            }
        }
        else if ((!fresh) && heartbeatNow - m_lastHeartbeatWarnMs >= 5 * 60'000)
        {
            m_lastHeartbeatWarnMs = heartbeatNow;
            appendLog(QStringLiteral(
                "[外部信号] xau-5m 心跳已过期/未检测到；"
                "请检查新 Claude 会话里的 /loop 5m /xau-5m。"));
        }
    }

    const QStringList inboxes =
        pulse::ui::ExternalSignalStore::pendingSignalPaths();
    if (inboxes.isEmpty())
    {
        return;
    }

    for (const QString &inbox : inboxes)
    {
        pulse::ui::ExternalSignal signal;
        const auto result =
            pulse::ui::ExternalSignalStore::readSignalAt(inbox, signal);
        if (pulse::ui::ExternalSignalStore::ReadResult::Missing == result)
        {
            continue;
        }
        if (pulse::ui::ExternalSignalStore::ReadResult::Invalid == result)
        {
            if (signal.signalId.isEmpty())
            {
                // No id to de-duplicate on; log once per distinct parse error
                // so a half-written file cannot flood the log at 1 Hz.
                if (m_lastSeenSignalId != QStringLiteral("<invalid>")
                    || m_lastInvalidSignalError != signal.error)
                {
                    m_lastSeenSignalId = QStringLiteral("<invalid>");
                    m_lastInvalidSignalError = signal.error;
                    appendLog(QStringLiteral("[外部信号] %1 忽略无效信号：%2")
                                  .arg(signal.strategyId, signal.error));
                }
                continue;
            }
            if (m_processedSignalIds.count(signal.signalId) > 0)
            {
                continue;
            }
            m_lastSeenSignalId = signal.signalId;
            appendLog(QStringLiteral("[5分钟分析] %1 无效信号 %2：%3")
                          .arg(signal.strategyId, signal.signalId, signal.error));
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("INVALID"), signal.error);
            return;
        }

        if (m_processedSignalIds.count(signal.signalId) > 0)
        {
            if (m_lastSeenSignalId != signal.signalId)
            {
                m_lastSeenSignalId = signal.signalId;
                appendLog(QStringLiteral("[外部信号] %1 忽略重复 signal_id：%2")
                              .arg(signal.strategyId, signal.signalId));
            }
            continue;
        }

        processExternalSignal(signal);
        return;
    }
}

void SimGateSystem::processExternalSignal(const pulse::ui::ExternalSignal &signal)
{
    if (m_lastSeenSignalId != signal.signalId)
    {
        m_lastSeenSignalId = signal.signalId;
        const QString target = signal.targets.isEmpty()
                                   ? QStringLiteral("--")
                                   : QString::number(signal.targets.first(), 'f', 2);
        appendLog(QStringLiteral("[5分钟分析] %1 action=%2 stance=%3 ref=%4 stop=%5 TP=%6")
                      .arg(signal.round, signal.action, signal.stance)
                      .arg(signal.referencePrice, 0, 'f', 2)
                      .arg(signal.stop, 0, 'f', 2)
                      .arg(target));
        if (false == signal.reason.isEmpty())
        {
            appendLog(QStringLiteral("[5分钟分析] 理由：%1").arg(signal.reason));
        }
        if (false == signal.analysis.isEmpty())
        {
            QString analysis = signal.analysis.trimmed();
            if (analysis.size() > 800)
            {
                analysis = analysis.left(800) + QStringLiteral("…");
            }
            appendLog(QStringLiteral("[v10精选 每秒执行] %1 %2")
                          .arg(signal.round)
                          .arg(analysis));
        }
    }

    const QDateTime now = QDateTime::currentDateTime();

    // ---- source / mode / symbol ------------------------------------------
    if (signal.source != QStringLiteral("gold-trader-mind"))
    {
        finishExternalSignal(signal, QStringLiteral("rejected"),
                             QStringLiteral("SOURCE"), QStringLiteral("未知信号来源：%1").arg(signal.source));
        return;
    }
    if (signal.mode != QStringLiteral("paper") && signal.mode != QStringLiteral("shadow"))
    {
        finishExternalSignal(signal, QStringLiteral("rejected"),
                             QStringLiteral("MODE"), QStringLiteral("不支持的 mode：%1").arg(signal.mode));
        return;
    }
    if (signal.symbol != QStringLiteral("XAUUSD"))
    {
        finishExternalSignal(signal, QStringLiteral("rejected"),
                             QStringLiteral("SYMBOL"), QStringLiteral("不支持的品种：%1").arg(signal.symbol));
        return;
    }

    // A shadow signal is a dry run: log/ack it, but never touch the account.
    if (signal.mode == QStringLiteral("shadow"))
    {
        finishExternalSignal(signal, QStringLiteral("accepted"),
                             QStringLiteral("SHADOW"),
                             QStringLiteral("shadow 模式：只校验/记录，不落单"));
        return;
    }

    // ---- timestamps ------------------------------------------------------
    if (signal.createdAt.msecsTo(now) < -60'000)
    {
        finishExternalSignal(signal, QStringLiteral("rejected"),
                             QStringLiteral("FUTURE"), QStringLiteral("created_at 超前当前时间"));
        return;
    }
    if (signal.action == QStringLiteral("open"))
    {
        if (now > signal.validUntil.addSecs(15)
            || signal.createdAt.secsTo(now) > 15 * 60)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("STALE"), QStringLiteral("信号已过期"));
            return;
        }
    }
    else if (signal.action == QStringLiteral("close") || signal.action == QStringLiteral("modify"))
    {
        if (now > signal.validUntil.addSecs(10 * 60))
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("STALE"), QStringLiteral("信号已过期"));
            return;
        }
    }

    if (signal.action == QStringLiteral("none"))
    {
        finishExternalSignal(signal, QStringLiteral("accepted"),
                             QStringLiteral("NO_TRADE"),
                             signal.reason.isEmpty() ? QStringLiteral("本轮无交易")
                                                     : signal.reason);
        return;
    }

    // No paper execution without a live analysis loop: xau-5m writes its
    // heartbeat immediately before the signal, so a legitimate current-round
    // signal always passes. A leftover paper signal from a stopped loop is
    // rejected instead of opening a position into a dead strategy.
    if (false == xau5mHeartbeatFresh())
    {
        finishExternalSignal(signal, QStringLiteral("rejected"),
                             QStringLiteral("XAU5M_DOWN"),
                             QStringLiteral("xau-5m 心跳缺失/过期，暂不执行；模拟盘继续轮询行情"));
        return;
    }

    // ---- market / price freshness ---------------------------------------
    if (false == m_marketStatusKnown.load(std::memory_order_acquire))
    {
        if (now <= signal.validUntil)
        {
            if (m_waitingPriceSignalId != signal.signalId)
            {
                m_waitingPriceSignalId = signal.signalId;
                appendLog(QStringLiteral("[外部信号] 等待 XAUUSD 行情状态后再执行…"));
            }
            return; // Retry on the next 1 s tick; do not mark as processed.
        }
        finishExternalSignal(signal, QStringLiteral("rejected"),
                             QStringLiteral("STALE"), QStringLiteral("行情状态未就绪且信号已过期"));
        return;
    }
    if (false == m_marketOpen.load(std::memory_order_acquire))
    {
        finishExternalSignal(signal, QStringLiteral("rejected"),
                             QStringLiteral("MARKET_CLOSED"),
                             QStringLiteral("XAUUSD 休市，不执行信号"));
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastPrice <= 0.0 || m_lastPriceMs <= 0 || nowMs - m_lastPriceMs > 10'000)
    {
        if (now <= signal.validUntil)
        {
            if (m_waitingPriceSignalId != signal.signalId)
            {
                m_waitingPriceSignalId = signal.signalId;
                appendLog(QStringLiteral("[外部信号] 等待有效的 XAUUSD 行情后再执行…"));
            }
            return; // Retry on the next 1 s tick; do not mark as processed.
        }
        finishExternalSignal(signal, QStringLiteral("rejected"),
                             QStringLiteral("STALE"), QStringLiteral("行情未就绪且信号已过期"));
        return;
    }
    m_waitingPriceSignalId.clear();

    const double fill = m_lastPrice;
    const bool isLong = (signal.stance == QStringLiteral("long"));

    if (signal.action == QStringLiteral("open"))
    {
        if (false == signal.triggerConfirmed)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("NOT_CONFIRMED"),
                                 QStringLiteral("open 信号未标记 trigger.confirmed"));
            return;
        }
        if (signal.stance != QStringLiteral("long") && signal.stance != QStringLiteral("short"))
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("STANCE"),
                                 QStringLiteral("open 信号 stance 必须是 long/short"));
            return;
        }
        if (std::abs(signal.lots - kExternalLots) > 1e-9
            || std::abs(signal.leverage - kExternalLeverage) > 1e-9)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("LOTS_LEVERAGE"),
                                 QStringLiteral("手数/杠杆必须是 0.01 / 1000（收到 %1 / %2）")
                                     .arg(signal.lots, 0, 'f', 2)
                                     .arg(signal.leverage, 0, 'f', 0));
            return;
        }
        if (signal.targets.isEmpty() || signal.stop <= 0.0)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("LEVELS"),
                                 QStringLiteral("open 信号缺少 stop/targets"));
            return;
        }

        const double maxDrift = std::min(kExternalMaxDriftCapUsd,
                                         std::max(0.1, signal.maxDriftUsd));
        if (std::abs(fill - signal.referencePrice) > maxDrift)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("DRIFT"),
                                 QStringLiteral("价格漂移 %1 USD > 允许 %2 USD")
                                     .arg(std::abs(fill - signal.referencePrice), 0, 'f', 2)
                                     .arg(maxDrift, 0, 'f', 2));
            return;
        }

        const double tp = signal.targets.first();
        const double sl = signal.stop;
        const bool levelsOk = isLong ? (sl < fill && fill < tp) : (tp < fill && fill < sl);
        if (false == levelsOk)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("LEVELS_CROSSED"),
                                 QStringLiteral("价位与当前价不相容（fill %1 TP %2 SL %3）")
                                     .arg(fill, 0, 'f', 2)
                                     .arg(tp, 0, 'f', 2)
                                     .arg(sl, 0, 'f', 2));
            return;
        }

        const double riskUsd = std::abs(fill - sl) * kExternalLots
            * SimTradeEngine::kOzPerLot;
        const auto accountBefore = SimAccountStore::load(m_apiKey);
        const double balanceBefore = accountBefore ? accountBefore->balanceUsdt : 0.0;
        const double riskPct = balanceBefore > 0.0 ? riskUsd / balanceBefore * 100.0 : 0.0;
        if (riskUsd > kExternalMaxRiskUsd)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("RISK_CAP"),
                                 QStringLiteral("单笔风险 %1 USD > 上限 %2 USD")
                                     .arg(riskUsd, 0, 'f', 2)
                                     .arg(kExternalMaxRiskUsd, 0, 'f', 2));
            return;
        }
        if (riskPct > 20.0)
        {
            appendLog(QStringLiteral("[外部信号] 风险提示：单笔风险 %1 USD，占余额 %2%")
                          .arg(riskUsd, 0, 'f', 2)
                          .arg(riskPct, 0, 'f', 2));
        }

        const auto positions = SimTradeEngine::positions(m_apiKey);
        const QString owner = signal.strategyId.isEmpty()
            ? QStringLiteral("v10") : signal.strategyId;
        std::vector<pulse::ui::SimPosition> sameStrategy;
        for (const auto &position : positions)
        {
            const QString positionOwner = position.strategyId.isEmpty()
                ? QStringLiteral("v10") : position.strategyId;
            if (positionOwner == owner)
            {
                sameStrategy.push_back(position);
            }
        }
        QString closedPositionId;
        double closedPnl = 0.0;
        if (false == sameStrategy.empty())
        {
            if (false == signal.closeExisting)
            {
                finishExternalSignal(signal, QStringLiteral("rejected"),
                                     QStringLiteral("POSITION_EXISTS"),
                                     QStringLiteral("策略 %1 已有模拟持仓 %2；open 被拒绝")
                                         .arg(owner, sameStrategy.front().id));
                return;
            }
            const auto selected = std::find_if(
                sameStrategy.begin(), sameStrategy.end(),
                [&signal](const pulse::ui::SimPosition &position) {
                    return signal.positionId.isEmpty()
                        || signal.positionId == position.id;
                });
            if (selected == sameStrategy.end())
            {
                finishExternalSignal(signal, QStringLiteral("rejected"),
                                     QStringLiteral("POSITION_MISMATCH"),
                                     QStringLiteral("指定持仓 %1 不存在于策略 %2")
                                         .arg(signal.positionId, owner));
                return;
            }

            // Pre-check the post-close balance before giving up the old
            // position: closing first and then failing to open would leave
            // the account flat unintentionally.
            const auto &old = *selected;
            const bool oldIsLong = (old.side != QStringLiteral("short"));
            const double closePnl = oldIsLong
                                        ? (fill - old.entryPrice) * old.volumeLots
                                              * SimTradeEngine::kOzPerLot
                                        : (old.entryPrice - fill) * old.volumeLots
                                              * SimTradeEngine::kOzPerLot;
            const double balanceAfterClose = balanceBefore + old.marginUsdt + closePnl;
            // Both sides pay commission on open.
            const double fee = kExternalLots * SimTradeEngine::kFeeUsdtPerLot;
            const double required = kExternalLots * SimTradeEngine::kOzPerLot
                * fill / kExternalLeverage + fee;
            if (balanceAfterClose < required)
            {
                finishExternalSignal(signal, QStringLiteral("rejected"),
                                     QStringLiteral("BALANCE"),
                                     QStringLiteral("平掉旧仓后余额 %1 仍不足以开新仓（需 %2）")
                                         .arg(balanceAfterClose, 0, 'f', 2)
                                         .arg(required, 0, 'f', 2));
                return;
            }

            const auto close = SimTradeEngine::closePosition(
                m_apiKey, old.id, fill, QStringLiteral("external_reverse"), nowMs);
            if (false == close.ok)
            {
                finishExternalSignal(signal, QStringLiteral("rejected"),
                                     QStringLiteral("CLOSE_FAILED"), close.error);
                return;
            }
            closedPositionId = close.positionId;
            closedPnl = close.realizedPnl;
            const QString reverseText =
                QStringLiteral("外部信号反手平仓 %1 @%2：盈亏 %3 USD")
                    .arg(close.positionId)
                    .arg(fill, 0, 'f', 2)
                    .arg(close.realizedPnl, 0, 'f', 2);
            appendTrade(QStringLiteral("[模拟] %1").arg(reverseText));
            appendLog(QStringLiteral("[模拟] %1").arg(reverseText));
            refreshSimBalance(fill);
            refreshSimPositions(fill);
        }

        const SimTradeEngine::OpenResult open = isLong
            ? SimTradeEngine::openLong(m_apiKey, kExternalLots, fill, tp, sl,
                                       kExternalLeverage, nowMs,
                                       signal.signalId, owner)
            : SimTradeEngine::openShort(m_apiKey, kExternalLots, fill, tp, sl,
                                        kExternalLeverage, nowMs,
                                        signal.signalId, owner);
        if (false == open.ok)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("OPEN_FAILED"), open.error,
                                 QString(), closedPositionId, fill, riskUsd, riskPct);
            return;
        }

        refreshSimBalance(fill);
        refreshSimPositions(fill);
        const QString message = QStringLiteral(
                                    "策略 %1 开%2 0.01 手 @%3 TP%4 SL%5 杠杆1000x；"
                                    "保证金 %6 USD 手续费 %7 USD；风险 %8 USD (%9%)")
                                    .arg(owner)
                                    .arg(isLong ? QStringLiteral("多") : QStringLiteral("空"))
                                    .arg(fill, 0, 'f', 2)
                                    .arg(tp, 0, 'f', 2)
                                    .arg(sl, 0, 'f', 2)
                                    .arg(open.marginUsdt, 0, 'f', 2)
                                    .arg(open.feeUsdt, 0, 'f', 2)
                                    .arg(riskUsd, 0, 'f', 2)
                                    .arg(riskPct, 0, 'f', 2);
        appendLog(QStringLiteral("[模拟] %1").arg(message));
        appendTrade(QStringLiteral("[模拟] %1").arg(message));
        finishExternalSignal(signal, QStringLiteral("accepted"), QStringLiteral("OK"),
                             message, open.positionId, closedPositionId, fill,
                             riskUsd, riskPct);
        return;
    }

    if (signal.action == QStringLiteral("close"))
    {
        const auto positions = SimTradeEngine::positions(m_apiKey);
        const QString owner = signal.strategyId.isEmpty()
            ? QStringLiteral("v10") : signal.strategyId;
        if (positions.empty())
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("NO_POSITION"),
                                 QStringLiteral("没有可平的模拟持仓"));
            return;
        }
        QString targetId = signal.positionId;
        const pulse::ui::SimPosition *target = nullptr;
        for (const auto &position : positions)
        {
            const QString positionOwner = position.strategyId.isEmpty()
                ? QStringLiteral("v10") : position.strategyId;
            if (positionOwner != owner)
            {
                continue;
            }
            if (targetId.isEmpty() || position.id == targetId)
            {
                target = &position;
                targetId = position.id;
                break;
            }
        }
        if (nullptr == target)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("NO_POSITION"),
                                 QStringLiteral("策略 %1 没有指定持仓：%2")
                                     .arg(owner, targetId));
            return;
        }

        const auto close = SimTradeEngine::closePosition(
            m_apiKey, targetId, fill, QStringLiteral("external_signal"), nowMs);
        if (false == close.ok)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("CLOSE_FAILED"), close.error,
                                 QString(), QString(), fill);
            return;
        }
        refreshSimBalance(fill);
        refreshSimPositions(fill);
        const QString message = QStringLiteral("平仓 %1 @%2：盈亏 %3 USD")
                                    .arg(close.positionId)
                                    .arg(fill, 0, 'f', 2)
                                    .arg(close.realizedPnl, 0, 'f', 2);
        appendLog(QStringLiteral("[模拟] 外部信号%1").arg(message));
        appendTrade(QStringLiteral("[模拟] 外部信号%1").arg(message));
        finishExternalSignal(signal, QStringLiteral("accepted"), QStringLiteral("OK"),
                             message, close.positionId, close.positionId, fill);
        return;
    }

    if (signal.action == QStringLiteral("modify"))
    {
        const auto positions = SimTradeEngine::positions(m_apiKey);
        const QString owner = signal.strategyId.isEmpty()
            ? QStringLiteral("v10") : signal.strategyId;
        if (positions.empty())
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("NO_POSITION"),
                                 QStringLiteral("没有可修改的模拟持仓"));
            return;
        }
        QString targetId = signal.positionId;
        const pulse::ui::SimPosition *target = nullptr;
        for (const auto &position : positions)
        {
            const QString positionOwner = position.strategyId.isEmpty()
                ? QStringLiteral("v10") : position.strategyId;
            if (positionOwner != owner)
            {
                continue;
            }
            if (targetId.isEmpty() || position.id == targetId)
            {
                target = &position;
                targetId = position.id;
                break;
            }
        }
        if (nullptr == target)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("NO_POSITION"),
                                 QStringLiteral("策略 %1 没有指定持仓：%2")
                                     .arg(owner, targetId));
            return;
        }

        const double tp = signal.targets.first();
        const double sl = signal.stop;
        const double riskUsd = std::abs(fill - sl) * kExternalLots
            * SimTradeEngine::kOzPerLot;
        const auto account = SimAccountStore::load(m_apiKey);
        const double balance = account ? account->balanceUsdt : 0.0;
        const double riskPct = balance > 0.0 ? riskUsd / balance * 100.0 : 0.0;
        if (riskUsd > kExternalMaxRiskUsd)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("RISK_CAP"),
                                 QStringLiteral("修改后单笔风险 %1 USD > 上限 %2 USD")
                                     .arg(riskUsd, 0, 'f', 2)
                                     .arg(kExternalMaxRiskUsd, 0, 'f', 2));
            return;
        }

        const auto update = SimTradeEngine::setStops(m_apiKey, targetId, tp, sl, fill);
        if (false == update.ok)
        {
            finishExternalSignal(signal, QStringLiteral("rejected"),
                                 QStringLiteral("MODIFY_FAILED"), update.error,
                                 targetId, QString(), fill, riskUsd, riskPct);
            return;
        }
        refreshSimBalance(fill);
        refreshSimPositions(fill);
        const QString message = QStringLiteral("修改 %1：TP %2 SL %3；风险 %4 USD (%5%)")
                                    .arg(targetId)
                                    .arg(tp, 0, 'f', 2)
                                    .arg(sl, 0, 'f', 2)
                                    .arg(riskUsd, 0, 'f', 2)
                                    .arg(riskPct, 0, 'f', 2);
        appendLog(QStringLiteral("[模拟] 外部信号%1").arg(message));
        appendTrade(QStringLiteral("[模拟] 外部信号%1").arg(message));
        finishExternalSignal(signal, QStringLiteral("accepted"), QStringLiteral("OK"),
                             message, targetId, QString(), fill, riskUsd, riskPct);
        return;
    }

    finishExternalSignal(signal, QStringLiteral("rejected"),
                         QStringLiteral("ACTION"), QStringLiteral("未知 action：%1").arg(signal.action));
}

void SimGateSystem::finishExternalSignal(const pulse::ui::ExternalSignal &signal,
                                         const QString &status,
                                         const QString &code,
                                         const QString &message,
                                         const QString &positionId,
                                         const QString &closedPositionId,
                                         double fillPrice,
                                         double riskUsd,
                                         double riskPct)
{
    if (false == signal.signalId.isEmpty()
        && m_processedSignalIds.insert(signal.signalId).second)
    {
        m_recentSignalIds.append(signal.signalId);
        while (m_recentSignalIds.size() > 100)
        {
            m_recentSignalIds.removeFirst();
        }
        m_lastProcessedSignalId = signal.signalId;
    }

    const QDateTime now = QDateTime::currentDateTime();
    const auto account = SimAccountStore::load(m_apiKey);
    const double balance = account ? account->balanceUsdt : 0.0;

    QJsonObject ack;
    ack[QLatin1String("schema")] = QStringLiteral("gold.exec/1/ack");
    ack[QLatin1String("signal_id")] = signal.signalId;
    ack[QLatin1String("round")] = signal.round;
    ack[QLatin1String("strategy_id")] = signal.strategyId.isEmpty()
        ? QStringLiteral("v10") : signal.strategyId;
    ack[QLatin1String("processed_at")] = now.toString(Qt::ISODate);
    ack[QLatin1String("status")] = status;
    ack[QLatin1String("code")] = code;
    ack[QLatin1String("message")] = message;
    ack[QLatin1String("action")] = signal.action;
    ack[QLatin1String("stance")] = signal.stance;
    ack[QLatin1String("filled_price")] = fillPrice;
    ack[QLatin1String("position_id")] = positionId;
    ack[QLatin1String("closed_position_id")] = closedPositionId;
    ack[QLatin1String("balance_usdt")] = balance;
    ack[QLatin1String("risk_usd")] = riskUsd;
    ack[QLatin1String("risk_pct")] = riskPct;
    ack[QLatin1String("reference_price")] = signal.referencePrice;
    ack[QLatin1String("stop")] = signal.stop;
    ack[QLatin1String("leverage")] = signal.leverage;
    ack[QLatin1String("lots")] = signal.lots;
    QJsonArray targets;
    for (double target : signal.targets)
    {
        targets.append(target);
    }
    ack[QLatin1String("targets")] = targets;

    const QString ackTarget = signal.ackPath.isEmpty()
        ? pulse::ui::ExternalSignalStore::ackPath() : signal.ackPath;
    pulse::ui::ExternalSignalStore::writeAckAt(ackTarget, ack);

    QJsonObject event = ack;
    event[QLatin1String("schema")] = QStringLiteral("pulsetrader.external_event/1");
    event[QLatin1String("event")] = QStringLiteral("signal");
    event[QLatin1String("ts")] = now.toString(Qt::ISODate);
    event[QLatin1String("reason")] = signal.reason;
    event[QLatin1String("report")] = signal.report;
    appendExternalJournal(event);

    writeExternalState(true);
    appendLog(QStringLiteral("[外部信号] %1 %2：%3")
                  .arg(status.toUpper(), code, message));
}

void SimGateSystem::writeExternalState(bool force)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (false == force && m_lastExternalStateMs > 0
        && nowMs - m_lastExternalStateMs < 5'000)
    {
        return;
    }

    const auto account = SimAccountStore::load(m_apiKey);
    const double balance = account ? account->balanceUsdt : 0.0;
    double floatingTotal = 0.0;
    double riskTotal = 0.0;

    QJsonArray positions;
    if (account.has_value())
    {
        for (const auto &position : account->positions)
        {
            const double floating = floatingPnl(position, m_lastPrice);
            floatingTotal += floating;
            riskTotal += std::abs(position.entryPrice - position.stopLoss)
                * position.volumeLots * SimTradeEngine::kOzPerLot;

            QJsonObject item;
            item[QLatin1String("position_id")] = position.id;
            item[QLatin1String("side")] = position.side;
            item[QLatin1String("strategy_id")] = position.strategyId.isEmpty()
                ? QStringLiteral("v10") : position.strategyId;
            item[QLatin1String("lots")] = position.volumeLots;
            item[QLatin1String("entry_price")] = position.entryPrice;
            item[QLatin1String("take_profit")] = position.takeProfit;
            item[QLatin1String("stop_loss")] = position.stopLoss;
            item[QLatin1String("leverage")] = position.leverage;
            item[QLatin1String("margin_usdt")] = position.marginUsdt;
            item[QLatin1String("floating_pnl_usd")] = floating;
            item[QLatin1String("risk_usd")] = std::abs(position.entryPrice - position.stopLoss)
                * position.volumeLots * SimTradeEngine::kOzPerLot;
            item[QLatin1String("opened_at_ms")] = static_cast<double>(position.openedAtMs);
            if (false == position.sourceSignalId.isEmpty())
            {
                item[QLatin1String("source_signal_id")] = position.sourceSignalId;
            }
            positions.append(item);
        }
    }

    QJsonArray recent;
    for (const QString &id : m_recentSignalIds)
    {
        recent.append(id);
    }

    const double equity = balance + floatingTotal;
    QJsonObject state;
    state[QLatin1String("schema")] = QStringLiteral("pulsetrader.external_state/1");
    state[QLatin1String("updated_at")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    state[QLatin1String("engine_running")] = m_polling;
    state[QLatin1String("market_open")] = m_marketOpen.load(std::memory_order_acquire);
    state[QLatin1String("last_price")] = m_lastPrice;
    state[QLatin1String("last_price_age_ms")] =
        m_lastPriceMs > 0 ? static_cast<double>(nowMs - m_lastPriceMs) : -1.0;
    state[QLatin1String("balance_usdt")] = balance;
    state[QLatin1String("equity_usdt")] = equity;
    state[QLatin1String("positions")] = positions;
    state[QLatin1String("last_processed_signal_id")] = m_lastProcessedSignalId;
    state[QLatin1String("recent_signal_ids")] = recent;
    state[QLatin1String("data_dir")] = pulse::ui::ExternalSignalStore::dataDir();

    QJsonObject risk;
    risk[QLatin1String("max_risk_usd")] = kExternalMaxRiskUsd;
    risk[QLatin1String("current_risk_usd")] = riskTotal;
    risk[QLatin1String("risk_pct")] = equity > 0.0 ? riskTotal / equity * 100.0 : 0.0;
    state[QLatin1String("paper_risk")] = risk;

    if (pulse::ui::ExternalSignalStore::writeState(state))
    {
        m_lastExternalStateMs = nowMs;
    }
}

void SimGateSystem::appendExternalJournal(const QJsonObject &event)
{
    pulse::ui::ExternalSignalStore::appendJournal(event);
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
    // Positions that remain open get their mark-to-market extremes updated
    // for this tick. Closed positions were folded in at their fill price by
    // checkAndClose() before the DB write.
    SimTradeEngine::trackExcursions(m_apiKey, price);
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

        QJsonObject event;
        event[QLatin1String("schema")] = QStringLiteral("pulsetrader.external_event/1");
        event[QLatin1String("event")] = QStringLiteral("settlement");
        event[QLatin1String("ts")] = QDateTime::currentDateTime().toString(Qt::ISODate);
        event[QLatin1String("position_id")] = close.positionId;
        event[QLatin1String("status")] = QStringLiteral("closed");
        event[QLatin1String("code")] = close.reason;
        event[QLatin1String("filled_price")] = close.exitPrice;
        event[QLatin1String("pnl_usd")] = close.realizedPnl;
        event[QLatin1String("message")] = QStringLiteral("TP/SL 自动结算");
        appendExternalJournal(event);
    }
    if (false == closes.empty())
    {
        refreshSimBalance(price);
        refreshSimPositions(price);
        writeExternalState(true);
    }
}

void SimGateSystem::refreshSimPositions(double price)
{
    QTableWidget *table = ui->table_simPositions;
    const auto positions = SimTradeEngine::positions(m_apiKey);

    if (positions.empty())
    {
        table->clearSpans();
        table->setRowCount(1);
        table->setSpan(0, 0, 1, table->columnCount());
        auto *item = new QTableWidgetItem(QStringLiteral("无模拟持仓"));
        item->setTextAlignment(Qt::AlignCenter);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(0, 0, item);
        return;
    }

    table->clearSpans();

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
        table->setRowCount(0);
        table->setRowCount(rowCount);
    }

    const int operationColumn = table->columnCount() - 1;
    for (int row = 0; row < rowCount; ++row)
    {
        const pulse::ui::SimPosition &position =
            positions[static_cast<std::size_t>(row)];
        const bool isLong = (position.side != QStringLiteral("short"));
        const double pnl = floatingPnl(position, price);

        const auto setCell = [table, row, rebuild](int column, const QString &text,
                                                   Qt::Alignment alignment) {
            QTableWidgetItem *item = rebuild ? nullptr : table->item(row, column);
            if (nullptr == item)
            {
                item = new QTableWidgetItem();
                item->setTextAlignment(alignment);
                table->setItem(row, column, item);
            }
            item->setText(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            return item;
        };

        // The name column mirrors 真实持仓: the instrument, with the position
        // id/source in the tooltip.
        QTableWidgetItem *nameItem =
            setCell(0, QStringLiteral("XAUUSD"), Qt::AlignLeft | Qt::AlignVCenter);
        nameItem->setData(Qt::UserRole, position.id);
        QString tooltip = QStringLiteral("模拟持仓 ID：%1").arg(position.id);
        tooltip += QStringLiteral("\n策略：%1")
                       .arg(position.strategyId.isEmpty()
                                ? QStringLiteral("v10") : position.strategyId);
        if (false == position.sourceSignalId.isEmpty())
        {
            tooltip += QStringLiteral("\n来源信号：%1").arg(position.sourceSignalId);
        }
        nameItem->setToolTip(tooltip);

        setCell(1, isLong ? QStringLiteral("多仓") : QStringLiteral("空仓"),
                Qt::AlignCenter);
        setCell(2, QString::number(position.volumeLots, 'f', 2),
                Qt::AlignRight | Qt::AlignVCenter);
        setCell(3, formatPositionPrice(position.entryPrice),
                Qt::AlignRight | Qt::AlignVCenter);
        setCell(4, price > 0.0 ? formatPositionPrice(price) : QStringLiteral("--"),
                Qt::AlignRight | Qt::AlignVCenter);
        setCell(5, formatPositionPrice(position.takeProfit),
                Qt::AlignRight | Qt::AlignVCenter);
        setCell(6, formatPositionPrice(position.stopLoss),
                Qt::AlignRight | Qt::AlignVCenter);
        setCell(7, QString::number(position.marginUsdt, 'f', 2),
                Qt::AlignRight | Qt::AlignVCenter);
        QTableWidgetItem *pnlItem =
            setCell(8, price > 0.0 ? formatSignedUsd(pnl) : QStringLiteral("--"),
                    Qt::AlignRight | Qt::AlignVCenter);
        if (price > 0.0 && pnl > 0.0)
        {
            pnlItem->setForeground(QColor(QStringLiteral("#c62828")));
        }
        else if (price > 0.0 && pnl < 0.0)
        {
            pnlItem->setForeground(QColor(QStringLiteral("#2e7d32")));
        }
        else
        {
            pnlItem->setData(Qt::ForegroundRole, QVariant());
        }

        if (rebuild)
        {
            auto *closeButton = new QPushButton(QStringLiteral("平仓"));
            connect(closeButton, &QPushButton::clicked, this,
                    [this, id = position.id]() { closeSimPosition(id); });
            table->setCellWidget(row, operationColumn, closeButton);
        }
        else
        {
            auto *closeButton =
                qobject_cast<QPushButton *>(table->cellWidget(row, operationColumn));
            if (nullptr != closeButton)
            {
                closeButton->setText(QStringLiteral("平仓"));
                closeButton->setEnabled(true);
            }
        }
    }
}

void SimGateSystem::closeSimPosition(const QString &positionId)
{
    if (m_lastPrice <= 0.0)
    {
        appendLog(QStringLiteral("[模拟] 手动平仓失败：暂无 XAUUSD 行情价格"));
        return;
    }
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto close = SimTradeEngine::closePosition(
        m_apiKey, positionId, m_lastPrice, QStringLiteral("manual_ui"), nowMs);
    if (false == close.ok)
    {
        appendLog(QStringLiteral("[模拟] 手动平仓失败：%1").arg(close.error));
        return;
    }
    const QString manualCloseText = QStringLiteral("手动平仓 %1 @%2：盈亏 %3 USD")
                                        .arg(close.positionId)
                                        .arg(close.exitPrice, 0, 'f', 2)
                                        .arg(close.realizedPnl, 0, 'f', 2);
    appendLog(QStringLiteral("[模拟] %1").arg(manualCloseText));
    appendTrade(QStringLiteral("[模拟] %1").arg(manualCloseText));

    QJsonObject event;
    event[QLatin1String("schema")] = QStringLiteral("pulsetrader.external_event/1");
    event[QLatin1String("event")] = QStringLiteral("settlement");
    event[QLatin1String("ts")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    event[QLatin1String("position_id")] = close.positionId;
    event[QLatin1String("status")] = QStringLiteral("closed");
    event[QLatin1String("code")] = QStringLiteral("MANUAL");
    event[QLatin1String("filled_price")] = close.exitPrice;
    event[QLatin1String("pnl_usd")] = close.realizedPnl;
    event[QLatin1String("message")] = QStringLiteral("模拟持仓表格手动平仓");
    appendExternalJournal(event);

    refreshSimBalance(m_lastPrice);
    refreshSimPositions(m_lastPrice);
    writeExternalState(true);
}


void SimGateSystem::onPollingStopped()
{
    m_polling = false;
    m_externalSignalTimer.stop();
    m_marketOpen.store(false, std::memory_order_release);
    m_marketStatusKnown.store(false, std::memory_order_release);
    m_waitingPriceSignalId.clear();
    updateEngineButtons(false);
    writeExternalState(true);
}

void SimGateSystem::updateEngineButtons(bool running)
{
    ui->pushBtn_startEngine->setEnabled(false == running);
    ui->pushBtn_stopEngine->setEnabled(running);
}

void SimGateSystem::pollLlmOverlay()
{
    const QString overlayPath =
        QFileInfo(pulse::ui::ExternalSignalStore::statePath()).absolutePath()
        + QStringLiteral("/llm_overlay.json");
    const QString auditPath =
        QFileInfo(pulse::ui::ExternalSignalStore::statePath()).absolutePath()
        + QStringLiteral("/llm_overlay_audit.jsonl");
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 nowMs = now.toMSecsSinceEpoch();
    const QFileInfo info(overlayPath);

    QString status;
    QString error;
    QJsonObject overlay;
    QByteArray raw;
    qint64 mtimeMs = 0;
    QDateTime generatedAt;
    QDateTime validUntil;

    if (false == info.exists())
    {
        status = QStringLiteral("missing");
        error = QStringLiteral("未找到 overlay 文件（Claude Code 5分钟分析未运行或尚未写入）");
    }
    else
    {
        mtimeMs = info.lastModified().toMSecsSinceEpoch();
        QFile file(overlayPath);
        if (false == file.open(QIODevice::ReadOnly))
        {
            status = QStringLiteral("invalid");
            error = QStringLiteral("无法读取 overlay 文件");
        }
        else
        {
            raw = file.readAll();
            file.close();
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
            if (parseError.error != QJsonParseError::NoError || false == doc.isObject())
            {
                status = QStringLiteral("invalid");
                error = QStringLiteral("JSON 解析失败：%1")
                            .arg(parseError.errorString());
            }
            else
            {
                overlay = doc.object();
                QStringList errors;
                if (overlay.value(QStringLiteral("schema")).toString()
                    != QStringLiteral("xau.llm_overlay/1"))
                {
                    errors << QStringLiteral("schema 不是 xau.llm_overlay/1");
                }
                if (overlay.value(QStringLiteral("symbol")).toString().toUpper()
                    != QStringLiteral("XAUUSD"))
                {
                    errors << QStringLiteral("symbol 不是 XAUUSD");
                }
                const QString bias =
                    overlay.value(QStringLiteral("bias")).toString().toLower();
                if (bias != QStringLiteral("long")
                    && bias != QStringLiteral("short")
                    && bias != QStringLiteral("neutral"))
                {
                    errors << QStringLiteral("bias 无效");
                }
                const QString regime =
                    overlay.value(QStringLiteral("regime")).toString().toLower();
                if (regime != QStringLiteral("trend")
                    && regime != QStringLiteral("range")
                    && regime != QStringLiteral("transition"))
                {
                    errors << QStringLiteral("regime 无效");
                }
                const double confidence =
                    overlay.value(QStringLiteral("confidence")).toDouble(-1.0);
                if (confidence < 0.0 || confidence > 1.0)
                {
                    errors << QStringLiteral("confidence 不在 0..1");
                }
                if (overlay.contains(QStringLiteral("support"))
                    && false == overlay.value(QStringLiteral("support")).isArray())
                {
                    errors << QStringLiteral("support 不是数组");
                }
                if (overlay.contains(QStringLiteral("resistance"))
                    && false == overlay.value(QStringLiteral("resistance")).isArray())
                {
                    errors << QStringLiteral("resistance 不是数组");
                }
                generatedAt = QDateTime::fromString(
                    overlay.value(QStringLiteral("generated_at")).toString(),
                    Qt::ISODate);
                validUntil = QDateTime::fromString(
                    overlay.value(QStringLiteral("valid_until")).toString(),
                    Qt::ISODate);
                if (false == generatedAt.isValid())
                {
                    errors << QStringLiteral("generated_at 缺失或格式错误");
                }
                if (false == validUntil.isValid())
                {
                    errors << QStringLiteral("valid_until 缺失或格式错误");
                }
                if (generatedAt.isValid() && validUntil.isValid()
                    && validUntil <= generatedAt)
                {
                    errors << QStringLiteral("valid_until 不晚于 generated_at");
                }
                if (generatedAt.isValid() && generatedAt > now.addSecs(120))
                {
                    errors << QStringLiteral("generated_at 超前超过 120 秒");
                }

                if (false == errors.isEmpty())
                {
                    status = QStringLiteral("invalid");
                    error = errors.join(QStringLiteral("；"));
                }
                else if (now > validUntil)
                {
                    status = QStringLiteral("stale");
                    error = QStringLiteral("overlay 已过期：valid_until=%1，当前=%2")
                                .arg(validUntil.toString(Qt::ISODate),
                                     now.toString(Qt::ISODate));
                }
                else if (generatedAt.secsTo(now) > 600)
                {
                    status = QStringLiteral("stale");
                    error = QStringLiteral("overlay 年龄 %1 秒 > 600 秒")
                                .arg(generatedAt.secsTo(now));
                }
                else
                {
                    status = QStringLiteral("ok");
                }
            }
        }
    }

    const QString signature = status + QLatin1Char('|')
        + overlay.value(QStringLiteral("generated_at")).toString()
        + QLatin1Char('|') + QString::number(mtimeMs);
    const bool changed = (signature != m_lastLlmOverlaySignature);
    const bool periodic = (nowMs - m_lastLlmOverlayLogMs >= 300'000);
    if (false == changed && false == periodic)
    {
        return;
    }
    m_lastLlmOverlaySignature = signature;
    m_lastLlmOverlayStatus = status;
    m_lastLlmOverlayLogMs = nowMs;

    // Persist a reviewable record even when the overlay is missing/invalid.
    QJsonObject audit;
    audit[QStringLiteral("at")] = now.toString(Qt::ISODate);
    audit[QStringLiteral("status")] = status;
    audit[QStringLiteral("error")] = error;
    audit[QStringLiteral("file_mtime_ms")] = static_cast<double>(mtimeMs);
    audit[QStringLiteral("last_price")] = m_lastPrice;
    audit[QStringLiteral("market_open")] = m_marketOpen.load();
    if (false == overlay.isEmpty())
    {
        audit[QStringLiteral("overlay")] = overlay;
    }
    else if (false == raw.isEmpty())
    {
        audit[QStringLiteral("raw")] =
            QString::fromUtf8(raw.left(2000));
    }
    if (const auto account = SimAccountStore::load(m_apiKey);
        account.has_value())
    {
        audit[QStringLiteral("balance_usdt")] = account->balanceUsdt;
        audit[QStringLiteral("positions")] =
            static_cast<int>(account->positions.size());
        if (false == account->positions.empty())
        {
            const auto &position = account->positions.front();
            audit[QStringLiteral("position_side")] = position.side;
            audit[QStringLiteral("position_entry")] = position.entryPrice;
        }
    }
    QFile auditFile(auditPath);
    if (auditFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        auditFile.write(QJsonDocument(audit).toJson(QJsonDocument::Compact));
        auditFile.write("\n");
        auditFile.close();
    }

    if (m_llmOverlayDialog != nullptr && m_llmOverlayDialog->isVisible())
    {
        m_llmOverlayDialog->reload();
    }
}

void SimGateSystem::appendLog(const QString &text)
{
    // UI log pane: only the deterministic v10-selected analysis is shown.
    // LLM overlay data lives in the separate LLM盘面 dialog and audit file.
    if (false == text.contains(QStringLiteral("v10精选")))
    {
        return;
    }
    // appendPlainText, not QTextEdit::append: the log carries strategy text
    // that could look like markup ("<" in a comparison, for instance), which
    // QTextEdit::append would render as HTML. The pane keeps the latest 5000
    // lines (maximumBlockCount, set in MainWindow.ui).
    ui->textEdit_log->appendPlainText(text);
}

void SimGateSystem::appendTrade(const QString &text)
{
    // QTextEdit::append() interprets its argument as rich text and trade
    // lines can carry "<" from error strings, so insert them as plain text
    // at the end of the document instead.
    QTextCursor cursor(ui->textEdit_trade->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text + QLatin1Char('\n'));
}

void SimGateSystem::loadClosedTradeHistory()
{
    const auto orders = pulse::ui::SimOrderDb::queryOrders(m_apiKey);
    std::vector<pulse::ui::OrderRecord> closed;
    closed.reserve(orders.size());
    for (const auto &order : orders)
    {
        if (order.status == QStringLiteral("closed"))
        {
            closed.push_back(order);
        }
    }
    if (closed.empty())
    {
        appendTrade(QStringLiteral("[历史] 暂无已平仓记录"));
        return;
    }

    // queryOrders() returns newest first; show the latest 50 oldest-first so
    // the pane reads chronologically and the newest line sits at the bottom.
    constexpr std::size_t kMaxLines = 50;
    const std::size_t count = std::min(closed.size(), kMaxLines);
    appendTrade(QStringLiteral("[历史] 已平仓交易 %1 笔（显示最近 %2 笔）")
                    .arg(closed.size())
                    .arg(count));
    for (std::size_t i = count; i > 0; --i)
    {
        const pulse::ui::OrderRecord &order = closed[i - 1];
        const QString side = (order.side == QStringLiteral("short"))
                                 ? QStringLiteral("空")
                                 : QStringLiteral("多");
        const QString pnlText =
            QStringLiteral("%1%2 USD")
                .arg(order.realizedPnl >= 0.0 ? QStringLiteral("+") : QString())
                .arg(order.realizedPnl, 0, 'f', 2);
        appendTrade(QStringLiteral("[历史] %1 %2 %3 手 | 开仓 %4 @%5 | 平仓 %6 @%7 | 盈亏 %8")
                        .arg(order.orderId, side)
                        .arg(order.volumeLots, 0, 'f', 2)
                        .arg(QString::number(order.entryPrice, 'f', 2))
                        .arg(order.openedAt)
                        .arg(QString::number(order.exitPrice, 'f', 2))
                        .arg(order.closedAt)
                        .arg(pnlText));
    }
}
