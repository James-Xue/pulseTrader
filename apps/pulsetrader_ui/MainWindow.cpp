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

#include <QDateTime>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QMetaObject>
#include <QStringList>

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

// Query the live XAUUSD CFD positions and apply the strategy stops
// (price ± points, side-aware) via putCfdPositionModify. Runs on the poll
// thread — throttled by the caller. Returns the per-position results.
std::vector<pulse::ui::RealStopsResult> followRealStopsOnce(
    pulse::exchange::GateRestClient &cfd, double tpPoints, double slPoints,
    double tolerance)
{
    std::vector<pulse::ui::RealStopsResult> results;
    const auto res = cfd.getCfdPositions();
    if (false == pulse::ok(res))
    {
        pulse::ui::RealStopsResult failure;
        failure.ok = false;
        failure.error = QString::fromStdString(pulse::error(res).message);
        results.push_back(failure);
        return results;
    }
    const nlohmann::json *obj = pulse::ui::VerifyWorker::unwrapData(
        pulse::ui::VerifyWorker::firstObject(pulse::value(res)));
    if (nullptr == obj || false == obj->contains("list") || false == (*obj)["list"].is_array())
    {
        pulse::ui::RealStopsResult failure;
        failure.ok = false;
        failure.error = QStringLiteral("持仓响应格式异常");
        results.push_back(failure);
        return results;
    }
    for (const auto &p : (*obj)["list"])
    {
        if (false == p.is_object() || "XAUUSD" != p.value("symbol", ""))
        {
            continue;
        }
        const double volume = safeField(p, "volume");
        const double entry = safeField(p, "price_open");
        if (volume <= 0.0 || entry <= 0.0)
        {
            continue;
        }
        const QString id = QString::number(
            static_cast<std::int64_t>(safeField(p, "position_id")));
        const bool isLong = ("Long" == p.value("position_dir", ""));
        // Strategy stops anchored on the ENTRY price (fixed point targets).
        const double tp = isLong ? entry + tpPoints : entry - tpPoints;
        const double sl = isLong ? entry - slPoints : entry + slPoints;

        // Skip when the exchange already carries the target stops (within
        // tolerance) — avoids pointless writes every poll cycle.
        const double curSl = safeField(p, "price_sl");
        const double curTp = safeField(p, "price_tp");
        if (tolerance > 0.0 && curSl > 0.0 && curTp > 0.0
            && std::abs(curSl - sl) <= tolerance && std::abs(curTp - tp) <= tolerance)
        {
            continue;
        }

        const auto modify = cfd.putCfdPositionModify(
            id.toStdString(), std::to_string(sl), std::to_string(tp));
        pulse::ui::RealStopsResult r;
        r.id = id;
        r.ok = pulse::ok(modify);
        if (false == r.ok)
        {
            r.error = QString::fromStdString(pulse::error(modify).message);
        }
        results.push_back(r);
    }
    return results;
}

} // namespace

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
    m_strategyParams->autoFollowReal.store(ui->check_autoFollow->isChecked());
    m_strategyParams->followIntervalSec.store(ui->spin_followInterval->value());
    m_strategyParams->followThresholdPoints.store(ui->spin_followThreshold->value());
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
        std::vector<pulse::ui::RealPosition> positions;
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        const auto res = cfd.getCfdPositions();
        if (pulse::ok(res))
        {
            const nlohmann::json *obj = pulse::ui::VerifyWorker::unwrapData(
                pulse::ui::VerifyWorker::firstObject(pulse::value(res)));
            if (nullptr != obj && obj->contains("list") && (*obj)["list"].is_array())
            {
                for (const auto &p : (*obj)["list"])
                {
                    if (false == p.is_object())
                    {
                        continue;
                    }
                    // Only gold positions get the strategy's gold stops.
                    if ("XAUUSD" != p.value("symbol", ""))
                    {
                        continue;
                    }
                    const double volume = safeField(p, "volume");
                    if (volume <= 0.0)
                    {
                        continue;
                    }
                    pulse::ui::RealPosition pos;
                    pos.id = QString::number(
                        static_cast<std::int64_t>(safeField(p, "position_id")));
                    pos.side = ("Long" == p.value("position_dir", ""))
                                   ? QStringLiteral("long")
                                   : QStringLiteral("short");
                    pos.volumeLots = volume;
                    pos.entryPrice = safeField(p, "price_open");
                    pos.priceSl = safeField(p, "price_sl");
                    pos.priceTp = safeField(p, "price_tp");
                    positions.push_back(pos);
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

void SimGateSystem::onRealPositionsFetched(const std::vector<pulse::ui::RealPosition> &positions)
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

void SimGateSystem::applyRealStops(const std::vector<pulse::ui::RealPosition> &positions)
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
            const auto res = cfd.putCfdPositionModify(
                pos.id.toStdString(), std::to_string(sl), std::to_string(tp));
            pulse::ui::RealStopsResult r;
            r.id = pos.id;
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
                    self->onRealStopsApplied(results, 0.0, false);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void SimGateSystem::onRealStopsApplied(const std::vector<pulse::ui::RealStopsResult> &results,
                                       double price, bool autoMode)
{
    const QString modeText = autoMode ? QStringLiteral("[跟随]") : QStringLiteral("[止损]");
    int okCount = 0;
    for (const auto &r : results)
    {
        if (r.ok)
        {
            okCount++;
            appendTrade(QStringLiteral("%1 持仓 %2：止盈止损已更新（按 %3 计算）。")
                            .arg(modeText, r.id)
                            .arg(price, 0, 'f', 2));
        }
        else
        {
            appendTrade(QStringLiteral("%1 持仓 %2：设置失败 — %3").arg(modeText, r.id, r.error));
        }
    }
    appendLog(QStringLiteral("%1 真实持仓止盈止损：%2/%3 成功。")
                  .arg(autoMode ? QStringLiteral("自动跟随") : QStringLiteral("手动设置"))
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
    // Real-position auto-follow shares the write-permission gate of the
    // manual button (a read-only key would be rejected by the exchange).
    const bool tradfiWritable = m_tradfiWritable;
    QPointer<SimGateSystem> self(this);
    m_pollThread = std::thread([self, cfg, params, tradfiWritable]() {
        // Auto-follow throttle state (poll-thread local).
        std::int64_t lastFollowTs = 0;
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

                        // Auto-follow (throttled): every followIntervalSec,
                        // if the price has moved at least followThresholdPoints
                        // since the last follow, re-apply the strategy stops
                        // to the LIVE XAUUSD positions (a real write — gated
                        // on the key's write permission).
                        if (tradfiWritable && params->autoFollowReal.load())
                        {
                            const std::int64_t intervalMs =
                                static_cast<std::int64_t>(params->followIntervalSec.load()) * 1000;
                            // Interval-gated check; the "trigger" parameter is
                            // the tolerance vs. the entry-anchored targets.
                            if (0 == lastFollowTs || nowMs - lastFollowTs >= intervalMs)
                            {
                                lastFollowTs = nowMs;
                                const auto results = followRealStopsOnce(
                                    cfd, params->tpPoints.load(), params->slPoints.load(),
                                    params->followThresholdPoints.load());
                                QMetaObject::invokeMethod(
                                    self,
                                    [self, results]() {
                                        if (self)
                                        {
                                            self->onRealStopsApplied(results, 0.0, true);
                                        }
                                    },
                                    Qt::QueuedConnection);
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
