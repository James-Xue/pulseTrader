#pragma once
// main_window.hpp — main window of the "模拟gate系统" launcher
//
// Pure Qt Designer form: every widget lives in MainWindow.ui and is reached
// through the ui pointer; no controls are created in code. The two buttons
// connect via the on_<objectName>_<signal> naming convention (Qt automatic
// connection — no manual connect() calls).
//
// Note: the class name is ASCII (SimGateSystem) because Qt's moc lexer only
// parses ASCII identifiers — the window title and all UI text stay Chinese
// ("模拟gate系统"), they are plain strings and are unaffected.
//
// Layout (see MainWindow.ui):
//   1. "账户信息：" label + balance label (label_balance) + 模拟账户 /
//      启动引擎 / 关闭引擎 buttons (horizontal row)
//   2. "模拟账户：" label + simulated balance (label_simBalance)
//   3. "日志信息：" label + read-only log QTextEdit  (textEdit_log)
//   4. "交易信息：" label + read-only trade QTextEdit (textEdit_trade)

#include <QWidget>

#include <atomic>
#include <memory>
#include <thread>

#include "GoldStrategyEngine.hpp"
#include "VerifyWorker.hpp"

namespace Ui
{
class SimGateSystem;
}

class SimGateSystem : public QWidget
{
    Q_OBJECT

  public:
    /// `login` carries the balance snapshot taken during credential
    /// verification — it is shown in the 账户信息 row (label_balance).
    /// `apiKey` identifies the simulated account (绑定到当前 key);
    /// `apiSecret` is used by the engine polling (REST client).
    explicit SimGateSystem(const pulse::ui::VerifyResult &login,
                           const QString &apiKey,
                           const QString &apiSecret,
                           QWidget *parent = nullptr);
    ~SimGateSystem() override;

  private slots:
    /// Auto-connected to the 模拟账户 button — create (initial balance) on
    /// first use, edit the balance afterwards.
    void on_pushBtn_simAccount_clicked();
    /// Auto-connected to the 启动引擎 button — starts the polling loop:
    /// every second, read the XAUUSD price, run the strategy engine and
    /// print the decision (open / TP / SL) into the trade pane.
    void on_pushBtn_startEngine_clicked();
    /// Auto-connected to the 关闭引擎 button — stops the polling loop.
    void on_pushBtn_stopEngine_clicked();
    /// Auto-connected to the 应用参数 button — push the spin-box values into
    /// the strategy params (atomic, hot-applied on the next poll round).
    void on_pushBtn_applyParams_clicked();

  private:
    /// Write the current spin-box values into m_strategyParams (atomic).
    void applyParamsFromUi();
    /// Show the strategy decision; on an open signal, open a simulated CFD
    /// position (margin = notional/leverage, real fee model).
    void handleDecision(const pulse::ui::GoldDecision &decision);
    /// Close TP/SL-hit positions (paper settlement) and refresh the panes.
    void settleSimPositions(double price);
    /// Re-render the 持仓 row from the persisted positions.
    void refreshSimPositions();
    /// Refresh label_simBalance from the persisted store.
    void refreshSimBalance();

    // -- Engine polling ----------------------------------------------------
    void startEnginePolling();
    void stopEnginePolling();
    void updateEngineButtons(bool running);
    /// UI-thread handler for one poll round (queued from the poll thread).
    void onPollResult(double price, const pulse::ui::GoldDecision &decision);
    void onPollingStopped();
    void appendLog(const QString &text);
    void appendTrade(const QString &text);

    Ui::SimGateSystem *ui;
    QString m_apiKey;
    QString m_apiSecret;

    /// Strategy tuning parameters — atomic fields, shared with the polling
    /// thread; the parameter UI writes here, the engine reads here.
    std::shared_ptr<pulse::ui::GoldStrategyEngine::Params> m_strategyParams;

    std::thread m_pollThread;             ///< Polling loop thread (joinable while running).
    std::atomic<bool> m_stopRequested{ false };
    bool m_polling = false;
};
