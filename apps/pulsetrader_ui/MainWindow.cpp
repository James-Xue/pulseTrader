// main_window.cpp — the constructor only runs ui->setupUi(this) plus the
// balance texts into label_balance / label_simBalance. The 模拟账户 flow and
// the engine polling (启动引擎 = a 1s XAUUSD price poll → GoldStrategyEngine
// decision) are the active logic; all widgets come from MainWindow.ui.

#include "MainWindow.hpp"

#include "EngineStrategyAdapter.hpp"
#include "SimAccountStore.hpp"
#include "SimTradeEngine.hpp"

#include "exchange/GateRestClient.hpp"

#include "ui_MainWindow.h"

#include <QInputDialog>
#include <QPointer>
#include <QMetaObject>
#include <QStringList>

#include <chrono>

using pulse::ui::SimAccountStore;
using pulse::ui::SimTradeEngine;

namespace
{

// Poll interval: 1 s. REST ticker polling faster than that risks rate
// limiting, and gold prices move on a minutes scale anyway.
constexpr auto kPollInterval = std::chrono::seconds(1);
// How often a persistent failure is reported to the log pane.
constexpr auto kErrorReportInterval = std::chrono::seconds(10);

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

    // Cap the panes so long-running polls cannot grow memory without bound
    // (QTextEdit has no block cap of its own — the document has).
    ui->textEdit_log->document()->setMaximumBlockCount(5000);
    ui->textEdit_trade->document()->setMaximumBlockCount(500);

    // Balance snapshot from the login verification, e.g.
    // "现货 USDT：0.006598  |  CFD：203.49 USD"
    QString balance = QStringLiteral("现货 USDT：%1")
                          .arg(QString::number(login.spotUsdt, 'f', 6));
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

    // Restore the simulated account and its open positions for this key.
    refreshSimBalance();
    refreshSimPositions();

    appendLog(QStringLiteral("[引擎] 就绪：点击“启动引擎”开始轮询 XAUUSD 行情并运行策略。"));
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
    m_strategyParams->tpVolMult.store(ui->spin_tpMult->value());
    m_strategyParams->slVolMult.store(ui->spin_slMult->value());
    m_strategyParams->openVolumeLots.store(ui->spin_openVolume->value());
    m_strategyParams->openLeverage.store(static_cast<double>(ui->spin_leverage->value()));
    m_strategyParams->cooldownMs.store(static_cast<std::int64_t>(ui->spin_cooldown->value()) * 1000);
    m_strategyParams->openProtectMin.store(ui->spin_protectMin->value());
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
    refreshSimBalance();
    appendLog(QStringLiteral("[模拟账户] 余额已设置：%1 USDT")
                  .arg(QString::number(value, 'f', 2)));
}

void SimGateSystem::refreshSimBalance()
{
    const auto account = SimAccountStore::load(m_apiKey);
    if (account.has_value())
    {
        ui->label_simBalance->setText(
            QStringLiteral("%1 USDT（更新于 %2）")
                .arg(QString::number(account->balanceUsdt, 'f', 2), account->updatedAt));
    }
    else
    {
        ui->label_simBalance->setText(QStringLiteral("未设置"));
    }
}

// ---------------------------------------------------------------------------
// Engine polling — 启动引擎: 1 s XAUUSD poll → GoldStrategyEngine → panes
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
    updateEngineButtons(true);
    appendLog(QStringLiteral("[引擎] 启动轮询：每 1 秒读取 XAUUSD 价格并执行策略。"));

    // The REST call blocks for up to ~10 s on network failures — never run
    // it on the UI thread. The poll thread posts results back via a
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
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        // The engine's real strategy (EmaResonanceScalper) drives the paper
        // trades; the adapter aggregates ticks into 1m candles and calls the
        // strategy's pure evaluateEntry() on every poll.
        pulse::ui::EngineStrategyAdapter adapter;
        const int warmupCandles = adapter.warmup(cfd);
        QMetaObject::invokeMethod(
            self,
            [self, warmupCandles]() {
                if (self)
                {
                    self->appendLog(warmupCandles > 0
                                        ? QStringLiteral("[策略] K线历史回填 %1 根，策略预热完成。")
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
                    marketOpen =
                        (false == obj->contains("status") || "open" == obj->value("status", "open"));

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
                        // Hot-read the tunable envelope parameters each round
                        // (atomic — the parameter UI writes them).
                        const double tpMult = params->tpVolMult.load();
                        const double slMult = params->slVolMult.load();
                        std::optional<pulse::ui::AdapterDecision> decision =
                            adapter.onTick(price, nowMs, tpMult, slMult,
                                           params->cooldownMs.load());
                        // Open-protection gate: evaluation keeps running (the
                        // kline stream stays continuous) but any entry signal
                        // inside the window is downgraded to "no open" — the
                        // first minutes of a fresh market carry unreliable
                        // prices (gaps/spikes), exactly the data the user
                        // does not want to trade on.
                        const std::int64_t protectMs =
                            static_cast<std::int64_t>(params->openProtectMin.load()) * 60'000;
                        if (decision.has_value() && decision->shouldOpen
                            && marketOpenSinceMs > 0
                            && nowMs - marketOpenSinceMs < protectMs)
                        {
                            decision->shouldOpen = false;
                            decision->reason =
                                QStringLiteral("开市保护期（剩余 %1 秒，开市初期数据不可靠）")
                                    .arg((protectMs - (nowMs - marketOpenSinceMs)) / 1000);
                        }
                        // Trailing stops: the envelope is recomputed every
                        // round regardless of the open decision (TP/SL =
                        // price ± mult × ATR from the buffered candles).
                        const std::optional<pulse::ui::EngineStrategyAdapter::Stops> stops =
                            adapter.suggestStops(price, tpMult, slMult);
                        QMetaObject::invokeMethod(
                            self,
                            [self, price, decision, stops]() {
                                if (self)
                                {
                                    self->onPollResult(price, decision, stops);
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

            // Interruptible sleep: check the stop flag every 100 ms. A null
            // self (window already destroyed) also exits the loop. While the
            // market is closed the poll slows to 60 s (1 s when open) — the
            // close state is re-checked on every wake-up.
            const int sleepChunks = marketOpen ? 10 : 600;
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
}

void SimGateSystem::stopEnginePolling()
{
    if (false == m_polling)
    {
        return;
    }
    m_stopRequested.store(true, std::memory_order_release);
    if (m_pollThread.joinable())
    {
        m_pollThread.join();  // Returns within ~100 ms (interruptible sleep).
    }
    m_polling = false;
    updateEngineButtons(false);
    appendLog(QStringLiteral("[引擎] 轮询已停止。"));
}

void SimGateSystem::onPollResult(
    double price, const std::optional<pulse::ui::AdapterDecision> &decision,
    const std::optional<pulse::ui::EngineStrategyAdapter::Stops> &stops)
{
    if (false == m_polling)
    {
        return;
    }
    appendLog(QStringLiteral("[行情] XAUUSD：%1 USD").arg(price, 0, 'f', 2));
    if (decision.has_value())
    {
        handleDecision(*decision);
    }
    settleSimPositions(price);
    // Trailing stops run AFTER settlement: a TP/SL hit closes first, then
    // the surviving positions get their stops moved up.
    applyTrailingStops(stops);
}

void SimGateSystem::applyTrailingStops(
    const std::optional<pulse::ui::EngineStrategyAdapter::Stops> &stops)
{
    if (false == stops.has_value())
    {
        return;  // Still warming up / no valid envelope — keep current stops.
    }
    const SimTradeEngine::StopsUpdateResult update =
        SimTradeEngine::updateStops(m_apiKey, stops->takeProfit, stops->stopLoss);
    if (update.updatedCount > 0)
    {
        refreshSimPositions();
        appendLog(QStringLiteral("[策略] 移动止盈止损：%1 个持仓更新为 TP %2 / SL %3")
                      .arg(update.updatedCount)
                      .arg(stops->takeProfit, 0, 'f', 2)
                      .arg(stops->stopLoss, 0, 'f', 2));
    }
}

void SimGateSystem::handleDecision(const pulse::ui::AdapterDecision &decision)
{
    if (false == decision.shouldOpen)
    {
        appendTrade(QStringLiteral("[策略] 不开仓：%1").arg(decision.reason));
        return;
    }

    // Volume and leverage come from the hot-tunable parameters (atomic).
    const double volumeLots = m_strategyParams->openVolumeLots.load();
    const double leverage = m_strategyParams->openLeverage.load();
    appendTrade(QStringLiteral("[策略] 开仓买多 %1 手 @%2 | 止盈 %3 | 止损 %4 | %5")
                    .arg(volumeLots, 0, 'f', 2)
                    .arg(decision.entryPrice, 0, 'f', 2)
                    .arg(decision.takeProfit, 0, 'f', 2)
                    .arg(decision.stopLoss, 0, 'f', 2)
                    .arg(decision.reason));

    // Paper trade with the real CFD model: margin = notional / leverage,
    // commission on the BUY side. No real order is ever placed.
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const SimTradeEngine::OpenResult open = SimTradeEngine::openLong(
        m_apiKey, volumeLots, decision.entryPrice, decision.takeProfit,
        decision.stopLoss, leverage, nowMs);
    if (open.ok)
    {
        refreshSimBalance();
        refreshSimPositions();
        appendLog(QStringLiteral("[模拟] 开仓 %1 手 @%2：保证金 %3 USDT + 手续费 %4 USDT，"
                                 "可用余额 %5 USDT（杠杆 %6x）")
                      .arg(volumeLots, 0, 'f', 2)
                      .arg(decision.entryPrice, 0, 'f', 2)
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
        refreshSimBalance();
        refreshSimPositions();
    }
}

void SimGateSystem::refreshSimPositions()
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
        parts << QStringLiteral("%1手@%2 多 TP%3 SL%4 保证金%5")
                     .arg(p.volumeLots, 0, 'f', 2)
                     .arg(p.entryPrice, 0, 'f', 2)
                     .arg(p.takeProfit, 0, 'f', 2)
                     .arg(p.stopLoss, 0, 'f', 2)
                     .arg(p.marginUsdt, 0, 'f', 2);
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
    ui->textEdit_log->append(text);
}

void SimGateSystem::appendTrade(const QString &text)
{
    ui->textEdit_trade->append(text);
}
