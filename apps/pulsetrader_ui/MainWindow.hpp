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
//   2. "模拟账户：" label + simulated balance (label_simBalance) and
//      "模拟持仓：" label + the paper positions (label_simPositions)
//   3. "真实持仓：" label + QTableWidget (table_realPositions) — the LIVE CFD
//      positions from GET /tradfi/positions: symbol, side, lots, entry price,
//      current (mark) price, take-profit, stop-loss, floating PnL (USD) and a
//      per-row 平仓 button that market-closes the position. The TP/SL cells
//      are editable: a committed value is PUT to the real Gate position.
//      Refreshed every 1 s from window construction, independent of the
//      engine poll
//   4. Lower area — two side-by-side columns:
//      left:  three stacked price tape panes, each a label + read-only
//             QPlainTextEdit, latest 50 lines: "黄金（XAUUSD）："
//             (textEdit_xau), "美元指数（USIDX）：" (textEdit_usidx) and
//             "XBRUSD：" (textEdit_xbrusd), top to bottom
//      right: "日志信息：" label + read-only log QPlainTextEdit
//             (textEdit_log, latest 5000 lines) above "交易信息：" label +
//             read-only trade QTextEdit (textEdit_trade)

#include <QWidget>
#include <QTimer>
#include <QStringList>
#include <QJsonObject>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <thread>

#include "EngineStrategyAdapter.hpp"
#include "ExternalSignalStore.hpp"
#include "GoldStrategyEngine.hpp"
#include "SimAccountStore.hpp"
#include "VerifyWorker.hpp"

namespace Ui
{
class SimGateSystem;
}

class QTableWidgetItem;

namespace pulse::ui
{

/// Outcome of one real position's stop modification.
struct RealStopsResult
{
    QString id;
    bool ok = false;
    QString error;
};

class TradeHistoryDialog;
class LlmOverlayDialog;

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

    /// Start the polling engine if it is not already running. Used by the
    /// --auto-start-engine launcher flag.
    void startEngine();

  private slots:
    /// Auto-connected to the 模拟账户 button — create (initial balance) on
    /// first use, reset the balance + open positions and clear the trade
    /// history afterwards.
    void on_pushBtn_simAccount_clicked();
    /// Auto-connected to the 查看历史交易数据 button — open (or raise) the
    /// non-modal trade-history dialog. The engine keeps polling while it is
    /// open.
    void on_pushBtn_tradeHistory_clicked();
    void on_pushBtn_llmOverlay_clicked();
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
    /// Auto-connected to the 启动黄金交易员 check box — the master arm switch
    /// for the automatic real-position stop strategy. Off (the default) means
    /// the window only displays positions/orders; nothing writes to the
    /// exchange automatically. Checking it also starts the engine poll if it
    /// is not already running (the strategy needs live quotes).
    void on_check_goldTrader_toggled(bool checked);
    /// Auto-connected to the strategy-number combo box next to the master
    /// switch — activate the selected 黄金交易员 strategy. Switching resets the
    /// peak/throttle state so the new strategy starts from a clean slate.
    void on_combo_goldTraderStrategy_currentIndexChanged(int index);
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

    // -- Real-CFD positions (真实持仓 table + 动态止盈止损) -------------------
    /// Fetch the real CFD positions on a background thread and repaint the
    /// 真实持仓 table. Runs once at startup and then every 1 s from
    /// m_realPositionsTimer — independent of the engine poll, so the table is
    /// populated even while the engine is stopped.
    void refreshRealPositions();
    /// UI-thread handler for refreshRealPositions(): rebuild the table from
    /// `positions`, or show `error` when the fetch / parse failed.
    void onRealPositionsUpdated(const std::vector<pulse::ui::CfdPosition> &positions,
                                const QString &error);
    /// Render a one-row placeholder (读取中 / 无持仓 / failure reason) across
    /// the whole 真实持仓 table.
    void showRealPositionsMessage(const QString &text);
    /// UI-thread handler for a committed edit of a 止盈/止损 cell: validate
    /// it and PUT the new protective prices to the REAL Gate position.
    void onRealPositionItemChanged(QTableWidgetItem *item);
    /// UI-thread handler for that PUT: log the outcome in the trade/log panes,
    /// report a failure, and refresh the table.
    void onRealStopsSynced(const QString &positionId, const QString &symbol,
                           double tp, double sl, bool ok, const QString &error);
    /// Market-close one LIVE CFD position immediately on click
    /// (POST /tradfi/positions/{id}/close, close_type=2 = full) on a
    /// background thread — no confirmation dialog. `silent` suppresses the
    /// modal failure dialog for the unattended peak-drawdown path.
    void closeRealPosition(const pulse::ui::CfdPosition &position,
                           bool silent = false);
    /// UI-thread handler for closeRealPosition(): log the outcome in the
    /// trade/log panes, report a failure (modal only when `silent` is false),
    /// and refresh the table immediately.
    void onRealPositionClosed(const QString &positionId, const QString &symbol,
                              bool ok, const QString &error, bool silent);

    // -- 黄金交易员 strategy roster ------------------------------------------
    /// One numbered 黄金交易员 strategy. The strategy-number combo box and the
    /// startup log are both generated from goldTraderStrategies(), so adding
    /// the next strategy is a single append there plus its dispatch in
    /// autoManageRealStops().
    struct GoldTraderStrategyInfo
    {
        int number = 0;  ///< 1-based strategy number shown in the combo box.
        QString name;    ///< Short display name.
        QString summary; ///< One-paragraph rule summary for the log.
    };
    /// The roster, numbered from 1 in display order (defined in the .cpp).
    static const std::vector<GoldTraderStrategyInfo> &goldTraderStrategies();
    /// Strategy number of the combo box selection (0 = nothing selected).
    int selectedGoldTraderStrategy() const;
    /// Log every strategy as "策略 N：名称 — 说明" plus the current selection.
    void logGoldTraderRoster();

    // -- Automatic real-position stops (1 m SuperTrend) ---------------------
    /// Manage the live XAUUSD stops from the latest position snapshot and the
    /// 1 m SuperTrend line. Runs on the UI thread once per engine poll only
    /// while BOTH the 启动黄金交易员 master switch and the 自动止损 switch are
    /// on; with the master off the window only displays positions/orders.
    ///
    ///   floating PnL <  0            → 8U loss stop
    ///   floating PnL > +2 (USD)      → break-even stop (entry ± 0.2)
    ///   0 ≤ PnL ≤ +2, no stop yet    → 8U protection stop
    ///   every positive PnL seen      → peak-PnL trail: the stop price
    ///                                  equivalent to (max PnL − 回撤U)
    ///                                  competes with the tier and SuperTrend
    ///                                  stops; the tightest wins
    ///   drawdown from peak ≥ 回撤U   → immediate market close (the trail stop
    ///                                  can no longer be placed)
    ///   every candidate              → compare with the 1 m SuperTrend line
    ///                                  placed 1U further in the loss
    ///                                  direction (pin-bar/wick protection);
    ///                                  use it when still better (long:
    ///                                  higher wins, short: lower wins)
    ///
    /// A stop is NEVER loosened: the tightest of the tier stop, the peak
    /// trail, the SuperTrend line and the stop already on the exchange is
    /// kept. Writes are throttled per position (更新间隔) and skipped for
    /// sub-threshold trailing moves (最小变动) and for unchanged values
    /// (Gate rejects no-op modifications with HTTP 400).
    void autoManageRealStops(double price, const pulse::ui::TickOutcome &outcome);
    /// Background PUT for autoManageRealStops(). Carries the position's
    /// current TP along so Gate does not clear it.
    void submitAutoRealStop(const pulse::ui::CfdPosition &position,
                            double stopPrice, const QString &reason);
    /// UI-thread handler for submitAutoRealStop(): log the outcome (no modal
    /// dialog — this path fires unattended) and keep the local snapshot in
    /// sync with what Gate accepted.
    void onAutoRealStopSynced(const QString &positionId, const QString &symbol,
                              double stopPrice, const QString &reason, bool ok,
                              const QString &error);

    /// Phase 1: query the real XAUUSD CFD positions (background thread).
    void fetchRealPositions();
    /// Phase 2 (UI thread): show the confirmation with the strategy stops.
    void onRealPositionsFetched(const std::vector<pulse::ui::CfdPosition> &positions);
    /// Phase 3: apply the strategy stops to the real positions (bg thread).
    void applyRealStops(const std::vector<pulse::ui::CfdPosition> &positions);
    /// Phase 4 (UI thread): report the modification results. `price` is the
    /// price the stops were computed from (0 when not meaningful).
    void onRealStopsApplied(const std::vector<pulse::ui::RealStopsResult> &results,
                            double price);
    /// Re-render the 模拟持仓 table from the persisted positions with the
    /// floating PnL of each position at `price`.
    void refreshSimPositions(double price);
    /// Manual close of one simulated position from the table's 平仓 button.
    /// The external strategy's signal remains the normal exit path; this is a
    /// deliberate operator override.
    void closeSimPosition(const QString &positionId);
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
    void onGoldTick(double price);

    /// External 5-minute analysis: read external_signal.json, validate it and
    /// execute the paper action on the UI thread (1 Hz while the engine runs).
    void onExternalSignalTimer();
    /// Claude Code advisory overlay: log a 5-minute status line and append a
    /// reviewable audit record. Analysis only; never opens/changes a position.
    void pollLlmOverlay();
    void processExternalSignal(const pulse::ui::ExternalSignal &signal);
    void finishExternalSignal(const pulse::ui::ExternalSignal &signal,
                              const QString &status,
                              const QString &code,
                              const QString &message,
                              const QString &positionId = QString(),
                              const QString &closedPositionId = QString(),
                              double fillPrice = 0.0,
                              double riskUsd = 0.0,
                              double riskPct = 0.0);
    /// Write external_state.json (5 s heartbeat unless `force`).
    void writeExternalState(bool force = false);
    /// True when xau_5m_heartbeat.json is present and younger than 12 min.
    bool xau5mHeartbeatFresh() const;
    /// Append one event line to external_signals.jsonl.
    void appendExternalJournal(const QJsonObject &event);
    void onPollingStopped();
    void appendLog(const QString &text);
    void appendTrade(const QString &text);
    /// Load the closed-trade history from sim_orders.db into the trade pane
    /// at startup: entry/exit price and time, PnL, newest at the bottom.
    void loadClosedTradeHistory();

    Ui::SimGateSystem *ui;
    QString m_apiKey;
    QString m_apiSecret;

    /// Non-modal history window, created on first use. Owned by the Qt parent
    /// (`this`), so it is destroyed with the main window.
    pulse::ui::TradeHistoryDialog *m_tradeHistoryDialog = nullptr;
    pulse::ui::LlmOverlayDialog *m_llmOverlayDialog = nullptr;

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
    /// Last successful XAUUSD tick (Unix ms) — external signals require a
    /// fresh price (<=10 s) before they may open/close/modify.
    qint64 m_lastPriceMs = 0;
    /// Set by the poll thread: the TradFi ticker status is open.
    std::atomic<bool> m_marketOpen{ false };
    /// True once the poll thread has read a ticker status at least once.
    std::atomic<bool> m_marketStatusKnown{ false };

    // -- External 5-minute analysis signal ----------------------------------
    /// Reads external_signal.json once per second while the engine polls.
    QTimer m_externalSignalTimer;
    /// Signal ids already processed (accepted or rejected). Loaded from
    /// external_state.json's recent_signal_ids on construction.
    std::set<QString> m_processedSignalIds;
    QStringList m_recentSignalIds;       ///< Newest last, capped at 100.
    QString m_lastProcessedSignalId;     ///< For external_state.json.
    QString m_lastSeenSignalId;          ///< Suppresses duplicate log lines.
    QString m_lastInvalidSignalError;    ///< Suppresses repeated invalid-JSON logs.
    qint64 m_lastExternalStateMs = 0;    ///< 5 s state heartbeat throttle.
    qint64 m_lastHeartbeatCheckMs = 0;   ///< 30 s xau-5m heartbeat check.
    qint64 m_lastHeartbeatWarnMs = 0;    ///< 5 min warning throttle.
    bool m_xau5mHeartbeatFresh = false;  ///< Last liveness check result.

    // -- Claude Code advisory overlay (analysis only, no orders) -------------
    QTimer m_llmOverlayTimer;
    QString m_lastLlmOverlaySignature;
    QString m_lastLlmOverlayStatus;
    qint64 m_lastLlmOverlayLogMs = 0;
    /// Set once when we start waiting for a fresh price for a pending signal,
    /// so the UI logs the wait once instead of every second.
    QString m_waitingPriceSignalId;

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

    // -- Real CFD positions table (真实持仓) --------------------------------
    /// Fires every 1 s from window construction — the position table and its
    /// floating PnL stay current even when the engine is not running.
    QTimer m_realPositionsTimer;
    /// True while a background GET /tradfi/positions is in flight; a timed
    /// refresh that fires during a slow request is skipped instead of piling
    /// up threads.
    std::atomic<bool> m_realPositionsFetching{ false };
    /// Exchange position ids whose full-close request is in flight — their
    /// row button shows 平仓中 and ignores further clicks until the result
    /// arrives (UI thread only).
    std::set<QString> m_closingPositionIds;
    /// True while onRealPositionsUpdated()/showRealPositionsMessage() write
    /// into the table, so itemChanged does not mistake those writes for user
    /// edits (UI thread only).
    bool m_updatingRealPositionsTable = false;
    /// Exchange position ids whose TP/SL PUT is in flight — at most one edit
    /// per row until the exchange answers (UI thread only).
    std::set<QString> m_modifyingPositionIds;

    // -- Automatic stop manager state (UI thread only) ----------------------
    /// Latest successful GET /tradfi/positions snapshot — the input of
    /// autoManageRealStops(). Kept fresh by every 1 s position refresh, even
    /// while an in-cell editor pauses the table repaint.
    std::vector<pulse::ui::CfdPosition> m_realPositions;
    /// Unix ms of that snapshot; 0 marks it stale (the last fetch failed), so
    /// the manager never acts on values it can no longer re-confirm.
    std::int64_t m_realPositionsUpdatedMs = 0;
    /// Position ids whose AUTOMATIC stop PUT is in flight (kept separate from
    /// m_modifyingPositionIds, whose failures raise a modal dialog).
    std::set<QString> m_autoStopInFlight;
    /// Per-position write throttle / duplicate-write guard + peak-PnL trail.
    struct AutoStopState
    {
        std::int64_t lastAttemptMs = 0; ///< Last auto write submitted (Unix ms).
        double lastSentSl = 0.0;        ///< Last target sent (0 = none).
        std::int64_t lastSentMs = 0;    ///< When it was sent (Unix ms).
        std::int64_t lastWarningMs = 0; ///< Last "8U stop unreachable" log.
        /// Highest floating PnL (USD) observed while the position was in
        /// profit. Updated every engine poll, before the write throttle.
        double peakPnl = 0.0;
        /// True once any positive floating PnL has been observed.
        bool hasPositivePeak = false;
        /// Last peak-drawdown market close attempt (Unix ms); retry throttle.
        std::int64_t lastPeakCloseMs = 0;
    };
    std::map<QString, AutoStopState> m_autoStopStates;

    QString m_spotBalanceText;            ///< "现货 USDT：x" (login snapshot, static).
    /// Whether the login key has CFD (gold) WRITE permission — modifying
    /// position stops mirrors the real exchange, which rejects read-only
    /// keys.
    bool m_tradfiWritable = true;
};
