// main_window.cpp — the constructor only runs ui->setupUi(this) plus the
// balance texts into label_balance / label_simBalance. The 模拟账户 flow and
// the engine polling (启动引擎 = a 1s XAUUSD price poll → GoldStrategyEngine
// decision) are the active logic; all widgets come from MainWindow.ui.

#include "MainWindow.hpp"

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
    appendLog(QStringLiteral("[参数] 已应用：EMA%1/%2 波幅窗%3 TP×%4 SL×%5 手数%6 "
                             "波幅[%7,%8] 冷却%9s")
                  .arg(ui->spin_emaFast->value())
                  .arg(ui->spin_emaSlow->value())
                  .arg(ui->spin_volWindow->value())
                  .arg(ui->spin_tpMult->value(), 0, 'f', 1)
                  .arg(ui->spin_slMult->value(), 0, 'f', 1)
                  .arg(ui->spin_openVolume->value(), 0, 'f', 2)
                  .arg(ui->spin_minVol->value(), 0, 'f', 2)
                  .arg(ui->spin_maxVol->value(), 0, 'f', 2)
                  .arg(ui->spin_cooldown->value())
                  .arg(QStringLiteral(" 杠杆%1x").arg(ui->spin_leverage->value())));
}

void SimGateSystem::applyParamsFromUi()
{
    // Atomic stores — visible to the polling thread on its next read.
    m_strategyParams->emaFast.store(ui->spin_emaFast->value());
    m_strategyParams->emaSlow.store(ui->spin_emaSlow->value());
    m_strategyParams->volWindow.store(ui->spin_volWindow->value());
    m_strategyParams->tpVolMult.store(ui->spin_tpMult->value());
    m_strategyParams->slVolMult.store(ui->spin_slMult->value());
    m_strategyParams->openVolumeLots.store(ui->spin_openVolume->value());
    m_strategyParams->minVolUsd.store(ui->spin_minVol->value());
    m_strategyParams->maxVolUsd.store(ui->spin_maxVol->value());
    m_strategyParams->cooldownMs.store(static_cast<std::int64_t>(ui->spin_cooldown->value()) * 1000);
    m_strategyParams->openLeverage.store(static_cast<double>(ui->spin_leverage->value()));
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
        pulse::ui::GoldStrategyEngine engine(params);
        std::chrono::steady_clock::time_point lastErrorReport{};
        bool firstErrorLogged = false;

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
                    if (price > 0.0)
                    {
                        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
                        const pulse::ui::GoldDecision decision =
                            engine.onPrice(price, nowMs);
                        QMetaObject::invokeMethod(
                            self,
                            [self, price, decision]() {
                                if (self)
                                {
                                    self->onPollResult(price, decision);
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
            // self (window already destroyed) also exits the loop.
            for (int i = 0; i < 10; ++i)
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

void SimGateSystem::onPollResult(double price, const pulse::ui::GoldDecision &decision)
{
    if (false == m_polling)
    {
        return;
    }
    appendLog(QStringLiteral("[行情] XAUUSD：%1 USD").arg(price, 0, 'f', 2));
    handleDecision(decision);
    settleSimPositions(price);
}

void SimGateSystem::handleDecision(const pulse::ui::GoldDecision &decision)
{
    if (false == decision.shouldOpen)
    {
        appendTrade(QStringLiteral("[策略] 不开仓：%1").arg(decision.reason));
        return;
    }

    appendTrade(QStringLiteral("[策略] 开仓买多 %1 手 @%2 | 止盈 %3 | 止损 %4 | %5")
                    .arg(decision.volumeLots, 0, 'f', 2)
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
        m_apiKey, decision.volumeLots, decision.entryPrice, decision.takeProfit,
        decision.stopLoss, m_strategyParams->openLeverage.load(), nowMs);
    if (open.ok)
    {
        refreshSimBalance();
        refreshSimPositions();
        appendLog(QStringLiteral("[模拟] 开仓 %1 手 @%2：保证金 %3 USDT + 手续费 %4 USDT，"
                                 "可用余额 %5 USDT（杠杆 %6x）")
                      .arg(decision.volumeLots, 0, 'f', 2)
                      .arg(decision.entryPrice, 0, 'f', 2)
                      .arg(open.marginUsdt, 0, 'f', 2)
                      .arg(open.feeUsdt, 0, 'f', 2)
                      .arg(SimAccountStore::load(m_apiKey)->balanceUsdt, 0, 'f', 2)
                      .arg(m_strategyParams->openLeverage.load(), 0, 'f', 0));
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
