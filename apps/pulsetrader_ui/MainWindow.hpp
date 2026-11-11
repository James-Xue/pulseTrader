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
//   3. Lower area — two side-by-side columns:
//      left:  three stacked price tape panes, each a label + read-only
//             QPlainTextEdit, latest 50 lines: "黄金（XAUUSD）："
//             (textEdit_xau), "美元指数（USIDX）：" (textEdit_usidx) and
//             "XBRUSD：" (textEdit_xbrusd), top to bottom
//      right: "日志信息：" label + read-only log QPlainTextEdit
//             (textEdit_log, latest 5000 lines) above "交易信息：" label +
//             read-only trade QTextEdit (textEdit_trade)

#include <QWidget>
#include <QTimer>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <thread>

#include "EngineStrategyAdapter.hpp"
#include "GoldStrategyEngine.hpp"
#include "SimAccountStore.hpp"
#include "VerifyWorker.hpp"

namespace Ui
{
class SimGateSystem;
}

namespace pulse::ui
{

/// A live CFD position from the exchange (动态止盈止损 targets).
struct RealPosition
{
    QString id;             ///< Exchange position id (stringified number).
    QString side;           ///< "long" / "short".
    double volumeLots = 0.0;
    double entryPrice = 0.0; ///< price_open — the TP/SL anchor (entry ± points).
    double priceSl = 0.0;    ///< Currently attached SL (0 if unknown).
    double priceTp = 0.0;    ///< Currently attached TP (0 if unknown).
};

/// Outcome of one real position's stop modification.
struct RealStopsResult
{
    QString id;
    bool ok = false;
    QString error;
};

} // namespace pulse::ui

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
    /// Auto-connected to the 订单分析 button — prints the post-trade report
    /// (per-order lifecycle + aggregates) into the trade pane.
    void on_pushBtn_analyze_clicked();
    /// Auto-connected to the 启动引擎 button — starts the polling threads:
    /// m_pollThread reads XAUUSD every second (drives the strategy engine and
    /// the decision printed into the trade pane, plus the XAUUSD tape pane)
    /// while m_usidxThread reads USIDX and m_xbrusdThread reads XBRUSD, both
    /// every second, each into its own reference pane. One thread per symbol
    /// so no symbol's REST latency delays another.
    void on_pushBtn_startEngine_clicked();
    /// Auto-connected to the 关闭引擎 button — stops the polling loop.
    void on_pushBtn_stopEngine_clicked();
    /// Auto-connected to the 应用参数 button — push the spin-box values into
    /// the strategy params (atomic, hot-applied on the next poll round).
    void on_pushBtn_applyParams_clicked();
    /// Auto-connected to the 动态止盈止损 button — re-apply the trailing
    /// stops to the EXISTING positions at the current price. Only adjusts
    /// stops on open positions; never opens a new trade.
    void on_pushBtn_dynamicStops_clicked();

  private:
    /// Write the current spin-box values into m_strategyParams (atomic).
    void applyParamsFromUi();
    /// Show the strategy decision; on an open signal, open a simulated CFD
    /// position (margin = notional/leverage, real fee model).
    void handleDecision(const std::optional<pulse::ui::AdapterDecision> &decision);
    /// Print the four-timeframe SuperTrend readout when any direction changed.
    void showTrendReadout(const pulse::ui::TickOutcome &outcome);
    /// Close TP/SL-hit positions (paper settlement) and refresh the panes.
    void settleSimPositions(double price);
    /// Apply trailing stops to open positions at `price` using the current
    /// point distances (favourable direction only).
    void applyTrailingStops(double price);

    // -- Real-CFD dynamic stops (动态止盈止损 on LIVE positions) -------------
    /// Phase 1: query the real XAUUSD CFD positions (background thread).
    void fetchRealPositions();
    /// Phase 2 (UI thread): show the confirmation with the strategy stops.
    void onRealPositionsFetched(const std::vector<pulse::ui::RealPosition> &positions);
    /// Phase 3: apply the strategy stops to the real positions (bg thread).
    void applyRealStops(const std::vector<pulse::ui::RealPosition> &positions);
    /// Phase 4 (UI thread): report the modification results. `price` is the
    /// price the stops were computed from; `autoMode` marks the throttled
    /// auto-follow (vs. the manual button).
    void onRealStopsApplied(const std::vector<pulse::ui::RealStopsResult> &results,
                            double price, bool autoMode);
    /// Re-render the 持仓 row from the persisted positions with the floating
    /// PnL of each position at `price`.
    void refreshSimPositions(double price);
    /// Refresh label_simBalance from the persisted store, including the
    /// floating PnL of open positions at `price` (equity = balance + PnL).
    void refreshSimBalance(double price);
    /// Floating PnL of one position at `price` (long/short aware).
    static double floatingPnl(const pulse::ui::SimPosition &position, double price);
    /// Refresh the 账户信息 row's CFD part from the LIVE account (equity and
    /// available balance from getCfdAssets, throttled by the poll thread).
    void refreshRealCfdBalance(double equityUsdt, double balanceUsdt);
    /// Append one "<Beijing time> <symbol>:<price>" line to that symbol's
    /// own pane — XAUUSD → textEdit_xau (top), USIDX → textEdit_usidx
    /// (middle), XBRUSD → textEdit_xbrusd (bottom). A rolling tape, not a
    /// replaced readout (the reference prices move too slowly for a single
    /// replaced number to look live). Each is printed at the exchange's own
    /// quote precision (XAUUSD/XBRUSD 2 decimals, USIDX 3). Each pane keeps
    /// the latest 50 lines (maximumBlockCount, set in MainWindow.ui).
    /// Display-only reference: nothing in the strategy path consumes it. The
    /// caller skips this on a failed fetch, so a closed session simply stops
    /// appending.
    void appendReferencePrice(const QString &symbol, double price);
    /// Once per minute while polling: compare each symbol's latest price with
    /// its price at the previous tick, print the direction triplet
    /// ("XAU:涨 USIDX:跌 XBRUSD:涨") into the log, and report which of
    /// USIDX/XBRUSD has the tightest rolling correlation with XAU (Pearson r
    /// over the last 30 per-minute price changes; |r| picks the tighter one,
    /// the sign says same/opposite direction). Runs on the UI thread only.
    void reportMarketRelations();

    // -- Engine polling ----------------------------------------------------
    void startEnginePolling();
    void stopEnginePolling();
    void updateEngineButtons(bool running);
    /// UI-thread handler for one poll round (queued from the poll thread).
    void onPollResult(double price, const pulse::ui::TickOutcome &outcome);
    void onPollingStopped();
    void appendLog(const QString &text);
    void appendTrade(const QString &text);

    Ui::SimGateSystem *ui;
    QString m_apiKey;
    QString m_apiSecret;

    /// Strategy tuning parameters — atomic fields, shared with the polling
    /// thread; the parameter UI writes here, the engine reads here.
    std::shared_ptr<pulse::ui::GoldStrategyEngine::Params> m_strategyParams;

    /// Gold price polling thread — the trading input. Fetches XAUUSD, drives
    /// the strategy adapter and the paper-trade settlement.
    std::thread m_pollThread;
    /// Reference-price polling threads — display-only. Each runs its own
    /// symbol fetch so that its REST latency cannot delay another symbol
    /// (serialising them in one loop halved the gold refresh rate). They
    /// share no mutable state with m_pollThread: they only post a price to
    /// the panel. All threads are joined by stopEnginePolling().
    std::thread m_usidxThread;
    std::thread m_xbrusdThread;
    /// Cooperative stop for ALL polling threads (write-once, read-only in
    /// the loops — the only state the threads share).
    std::atomic<bool> m_stopRequested{ false };
    // The EMA multi-timeframe panel was removed from the window on
    // 2026-09-23 (replaced by the 美元指数 panel). Its EmaRow renderer went
    // with it; the MultiTimeframeEma class and its tests stay in pulse_ui.
    bool m_polling = false;
    double m_lastPrice = 0.0;             ///< Latest polled gold price (UI thread).

    // -- Per-minute XAU/USIDX/XBRUSD relation report ------------------------
    /// One symbol's state for the relation report (UI thread only — price
    /// updates and the timer both live on the UI thread, so no locking).
    struct RelationState
    {
        double lastPrice = 0.0;        ///< Latest observed price.
        std::int64_t lastUpdateMs = 0; ///< Wall clock when lastPrice arrived.
        double minuteAnchor = 0.0;     ///< Price at the previous minute tick.
        bool hasPrice = false;
        bool hasMinuteAnchor = false;
    };
    /// Indices into m_relationStates — also the order used in the log line.
    enum RelationSymbol
    {
        kRelationXau = 0,
        kRelationUsidx = 1,
        kRelationXbrusd = 2,
        kRelationCount = 3,
    };
    std::array<RelationState, kRelationCount> m_relationStates{};
    /// Aligned per-minute price changes (only minutes where all three symbols
    /// produced a fresh delta), capped at 30 samples by reportMarketRelations.
    std::deque<double> m_xauDeltas;
    std::deque<double> m_usidxDeltas;
    std::deque<double> m_xbrusdDeltas;
    /// Fires once per minute while the engine polls; stopped with the engine.
    QTimer m_relationTimer;

    QString m_spotBalanceText;            ///< "现货 USDT：x" (login snapshot, static).
    /// Whether the login key has CFD (gold) WRITE permission — modifying
    /// position stops mirrors the real exchange, which rejects read-only
    /// keys.
    bool m_tradfiWritable = true;
};
