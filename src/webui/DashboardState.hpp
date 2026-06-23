#pragma once
// dashboard_state.hpp — Tiered polling aggregator for WebUI snapshots (Layer 9 WebUI)
//
// Assembles DashboardSnapshot from all upstream components at tiered intervals:
//   1. Fast   (200 ms) — order books, ticker prices
//   2. Medium (500 ms) — open positions, active orders
//   3. Slow   (1   s) — strategies, risk state, metrics
//   4. AI     (60   s) — AI analysis result (actual update every ~5 min)
//   5. Kline  (per candle) — new candle detection via open_time change
//
// Thread model:
//   - One std::jthread runs pollLoop() with a 50 ms sleep between checks
//   - Latest snapshot is stored behind a shared_mutex + shared_ptr for
//     lock-free readers (WebSocket broadcast thread)
//   - An optional SnapshotCallback is invoked after each poll cycle so the
//     WebSocket layer can push diffs to connected clients
//
// Thread safety:
//   - pollLoop() is the sole writer (single-threaded)
//   - latest() and the callback are readers (shared_mutex)
//   - m_running is atomic<bool> for lock-free status checks

#include "ai/AiPipeline.hpp"
#include "core/config.hpp"
#include "exchange/GateRestClient.hpp"
#include "execution/OrderTracker.hpp"
#include "market/MarketFeed.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/StrategyManager.hpp"
#include "webui/snapshot_types.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

namespace pulse::webui
{

// ---------------------------------------------------------------------------
// DashboardState — tiered polling aggregator
//
// Usage:
//   DashboardState state(config, market_feed, strategy_mgr, risk_mgr,
//                        order_tracker, ai_pipeline);
//   state.setSnapshotCallback([](auto snap) { broadcast(snap); });
//   state.start();
//   // ... later ...
//   auto snap = state.latest();
//   state.stop();
// ---------------------------------------------------------------------------
class DashboardState
{
  public:
    /// Construct with references to all upstream components.
    ///
    /// Does not start polling — call start() explicitly.
    ///
    /// Parameters:
    ///   1. config        — WebUI configuration (bind address, port, etc.)
    ///   2. market_feed   — L3 market data provider (ticker, orderbook, klines)
    ///   3. strategy_mgr  — L6 strategy engine (strategy snapshots)
    ///   4. risk_mgr      — L7 risk manager (risk snapshot, positions)
    ///   5. order_tracker — L8 order tracker (active orders, execution reports)
    ///   6. ai_pipeline   — L4 AI pipeline (analysis result)
    ///   7. rest_client   — L1 REST client (futures account balance, optional — may be null)
    ///   8. spot_rest     — L1 REST client (spot account balance, optional — may be null)
    DashboardState(const WebUiConfig &config,
                   market::MarketFeed &market_feed,
                   strategy::StrategyManager &strategy_mgr,
                   risk::RiskManager &risk_mgr,
                   execution::OrderTracker &order_tracker,
                   ai::AiPipeline &ai_pipeline,
                   exchange::GateRestClient *rest_client = nullptr,
                   exchange::GateRestClient *spot_rest_client = nullptr);

    /// Destructor — stops polling if still running.
    ~DashboardState();

    // Non-copyable, non-movable (owns a jthread).
    DashboardState(const DashboardState &) = delete;
    DashboardState &operator=(const DashboardState &) = delete;
    DashboardState(DashboardState &&) = delete;
    DashboardState &operator=(DashboardState &&) = delete;

    /// Callback type invoked after each poll cycle with the latest snapshot.
    ///
    /// The shared_ptr is immutable and safe to read from any thread.
    /// The WebSocket layer uses this to push diffs to connected clients.
    using SnapshotCallback = std::function<void(std::shared_ptr<const DashboardSnapshot>)>;

    /// Set the callback invoked after each poll cycle.
    ///
    /// Must be called before start() to avoid missing the first snapshot.
    void setSnapshotCallback(SnapshotCallback cb);

    /// Start the polling loop on a dedicated jthread.
    ///
    /// The thread runs pollLoop() with cooperative cancellation via stop_token.
    /// Has no effect if already running.
    void start();

    /// Stop the polling loop and join the thread.
    ///
    /// Signals the stop_token and blocks until the thread exits.
    /// Safe to call multiple times.
    void stop();

    /// Returns the most recent snapshot, or nullptr if no poll has completed yet.
    ///
    /// Thread-safe: takes a shared read lock.
    [[nodiscard]] std::shared_ptr<const DashboardSnapshot> latest() const;

    /// Returns true if the polling loop is currently running.
    ///
    /// Lock-free: reads an atomic<bool>.
    [[nodiscard]] bool running() const;

  private:
    /// Main polling loop running on a dedicated jthread.
    ///
    /// Algorithm:
    ///   1. Sleep for 50 ms
    ///   2. Check elapsed time since last poll at each tier
    ///   3. Dispatch to the appropriate poll_*() method
    ///   4. Assemble a new DashboardSnapshot and publish it
    ///   5. Check stop_token for cooperative cancellation
    void pollLoop(std::stop_token stoken);

    /// Fast tier (200 ms): order books + ticker prices.
    ///
    /// For each symbol from tickerCache().symbols():
    ///   - Retrieve top 20 bids/asks from orderbookManager()
    ///   - Store in snap.order_book
    void pollFast(DashboardSnapshot &snap);

    /// Medium tier (500 ms): open positions + active orders.
    ///
    /// Retrieves:
    ///   - risk_mgr.positionManager().getAllPositions()
    ///   - risk_mgr.positionManager().portfolioSummary()
    ///   - order_tracker.activeOrders()
    ///   - order_tracker.recentReports(10)
    void pollMedium(DashboardSnapshot &snap);

    /// Slow tier (1 s): strategies, risk state, metrics.
    ///
    /// Retrieves:
    ///   - strategy_mgr.snapshot()
    ///   - risk_mgr.riskSnapshot()
    ///   - metrics.available = false (not implemented yet)
    void pollSlow(DashboardSnapshot &snap);

    /// AI tier (60 s check, actual update ~5 min).
    ///
    /// Retrieves ai_pipeline.lastResult() and compares the shared_ptr
    /// for change detection (only updates when a new result is available).
    void pollAi(DashboardSnapshot &snap);

    /// Kline tier: per-candle change detection.
    ///
    /// For each symbol, retrieves the latest Kline from kline_buffer and
    /// compares open_time with the last known open_time. If changed, a new
    /// candle has formed and the kline snapshot is updated.
    void pollKlines(DashboardSnapshot &snap);

    /// Account tier (10 s): exchange-reported account balance.
    ///
    /// Fetches futures account balance via REST and stores in snap.account.
    /// Gracefully handles REST failures (sets available = false).
    void pollAccount(DashboardSnapshot &snap);

    // --- Configuration ---
    const WebUiConfig &m_config;

    // --- Upstream component references ---
    market::MarketFeed &m_marketFeed;
    strategy::StrategyManager &m_strategyMgr;
    risk::RiskManager &m_riskMgr;
    execution::OrderTracker &m_orderTracker;
    ai::AiPipeline &m_aiPipeline;
    exchange::GateRestClient *m_restClient; ///< Optional — futures REST client, may be null.
    exchange::GateRestClient *m_spotRestClient; ///< Optional — spot REST client, may be null.

    // --- Polling thread ---
    std::jthread m_pollThread;
    std::atomic<bool> m_running{ false };

    // --- Latest snapshot (shared read / exclusive write) ---
    mutable std::shared_mutex m_snapshotMutex;
    std::shared_ptr<const DashboardSnapshot> m_latestSnapshot{ nullptr };

    // --- Callback ---
    SnapshotCallback m_snapshotCallback;

    // --- Kline change detection state ---
    // Tracks open_time for new-candle detection and close price for
    // within-candle updates (Gate.io pushes OHLCV changes every ~2s).
    std::unordered_map<Symbol, std::int64_t> m_lastKlineOpenTimes;
    std::unordered_map<Symbol, double> m_lastKlineClose;

    // --- AI change detection state ---
    std::shared_ptr<const ai::AnalysisResult> m_lastAiResult{ nullptr };
};

} // namespace pulse::webui
