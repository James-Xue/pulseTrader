// dashboard_state.cpp — Tiered polling aggregator implementation (Layer 9 WebUI)
//
// Implements the pollLoop that dispatches to tiered poll methods at different
// intervals:
//   - Fast   (200 ms): order books + ticker prices
//   - Medium (500 ms): open positions + active orders
//   - Slow   (1   s):  strategies, risk state, metrics
//   - AI     (60   s): AI analysis result change detection
//   - Kline  (per candle): new candle detection via open_time change
//
// The loop sleeps 50 ms between checks to keep latency low while avoiding
// busy-waiting.

#include "webui/dashboard_state.hpp"

#include <algorithm>
#include <chrono>

namespace pulse::webui
{

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DashboardState::DashboardState(const WebUiConfig &config,
                               market::MarketFeed &market_feed,
                               strategy::StrategyManager &strategy_mgr,
                               risk::RiskManager &risk_mgr,
                               execution::OrderTracker &order_tracker,
                               ai::AiPipeline &ai_pipeline,
                               exchange::GateRestClient *rest_client,
                               exchange::GateRestClient *spot_rest_client)
    : m_config{ config }
    , m_marketFeed{ market_feed }
    , m_strategyMgr{ strategy_mgr }
    , m_riskMgr{ risk_mgr }
    , m_orderTracker{ order_tracker }
    , m_aiPipeline{ ai_pipeline }
    , m_restClient{ rest_client }
    , m_spotRestClient{ spot_rest_client }
{
}

DashboardState::~DashboardState()
{
    stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void DashboardState::setSnapshotCallback(SnapshotCallback cb)
{
    m_snapshotCallback = std::move(cb);
}

void DashboardState::start()
{
    // Guard: do not start if already running.
    if (m_running.load(std::memory_order_acquire))
    {
        return;
    }

    m_pollThread = std::jthread([this](std::stop_token stoken)
    {
        pollLoop(stoken);
    });
}

void DashboardState::stop()
{
    // Request cooperative cancellation.
    m_pollThread.request_stop();

    // Wait for the thread to finish (jthread joins on destruction, but we
    // want stop() to block until the thread has exited).
    if (m_pollThread.joinable())
    {
        m_pollThread.join();
    }
}

[[nodiscard]] std::shared_ptr<const DashboardSnapshot> DashboardState::latest() const
{
    std::shared_lock lock{ m_snapshotMutex };
    return m_latestSnapshot;
}

[[nodiscard]] bool DashboardState::running() const
{
    return m_running.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Poll loop
// ---------------------------------------------------------------------------

void DashboardState::pollLoop(std::stop_token stoken)
{
    using Clock = std::chrono::steady_clock;

    // Mark the thread as running.
    m_running.store(true, std::memory_order_release);

    // Track last-poll times for each tier.
    auto last_fast    = Clock::now();
    auto last_medium  = Clock::now();
    auto last_slow    = Clock::now();
    auto last_ai      = Clock::now();
    auto last_account = Clock::now();

    // Persistent snapshot that accumulates updates from each tier.
    // Only the changed panel is overwritten on each iteration; the rest
    // carries forward from the previous cycle.
    DashboardSnapshot snap;

    while (!stoken.stop_requested())
    {
        // 1. Sleep 50 ms between poll checks.
        std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });

        // Re-check stop condition after sleep.
        if (stoken.stop_requested())
        {
            break;
        }

        const auto now = Clock::now();
        bool updated = false;

        // 2. Fast tier: 200 ms — order books + tickers.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fast).count() >= 200)
        {
            pollFast(snap);
            pollKlines(snap);
            last_fast = now;
            updated = true;
        }

        // 3. Medium tier: 500 ms — positions + orders.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_medium).count() >= 500)
        {
            pollMedium(snap);
            last_medium = now;
            updated = true;
        }

        // 4. Slow tier: 1 s — strategies, risk, metrics.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_slow).count() >= 1000)
        {
            pollSlow(snap);
            last_slow = now;
            updated = true;
        }

        // 5. AI tier: 60 s — AI analysis change detection.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ai).count() >= 60000)
        {
            pollAi(snap);
            last_ai = now;
            updated = true;
        }

        // 6. Account tier: 10 s — exchange-reported account balance.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_account).count() >= 10000)
        {
            pollAccount(snap);
            last_account = now;
            updated = true;
        }

        // Also poll AI on the very first iteration so the dashboard has
        // an initial AI snapshot even before the 60 s interval fires.
        if (0 == snap.timestamp_ms)
        {
            pollAi(snap);
        }

        // 6. If any tier updated, stamp the snapshot and publish it.
        if (updated)
        {
            snap.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            // Create an immutable shared_ptr snapshot for readers.
            auto published = std::make_shared<const DashboardSnapshot>(snap);

            {
                std::unique_lock lock{ m_snapshotMutex };
                m_latestSnapshot = published;
            }

            // Invoke the callback (if set) outside the lock.
            if (m_snapshotCallback)
            {
                m_snapshotCallback(published);
            }
        }
    }

    // Mark the thread as no longer running.
    m_running.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// pollFast — 200 ms: order books + ticker prices
// ---------------------------------------------------------------------------

void DashboardState::pollFast(DashboardSnapshot &snap)
{
    // 1. Get all tracked symbols from the ticker cache.
    const auto symbols = m_marketFeed.tickerCache().symbols();

    // 2. For the first symbol (primary), retrieve top 20 bids/asks.
    //    The dashboard currently shows one symbol's order book at a time.
    if (!symbols.empty())
    {
        const auto &symbol = symbols[0];
        auto &ob_mgr = m_marketFeed.orderbookManager();

        snap.order_book.symbol = symbol;
        snap.order_book.bids = ob_mgr.topBids(symbol, 20);
        snap.order_book.asks = ob_mgr.topAsks(symbol, 20);

        // Retrieve sequence_id and timestamp from the full book if available.
        const auto book_opt = ob_mgr.get(symbol);
        if (book_opt.has_value())
        {
            snap.order_book.sequence_id = book_opt->sequence_id;
            snap.order_book.timestamp = book_opt->timestamp;
        }
    }
}

// ---------------------------------------------------------------------------
// pollKlines — per candle change detection
// ---------------------------------------------------------------------------

void DashboardState::pollKlines(DashboardSnapshot &snap)
{
    // 1. Get all tracked symbols.
    const auto symbols = m_marketFeed.tickerCache().symbols();

    if (symbols.empty())
    {
        return;
    }

    // 2. For the primary symbol, check if a new candle has formed.
    const auto &symbol = symbols[0];

    auto &kline_buf = m_marketFeed.getKlineBuffer(symbol);
    const auto latest_kline = kline_buf.latest();

    if (!latest_kline.has_value())
    {
        return;
    }

    // 3. Compare open_time and close price to detect new candles or within-candle updates.
    //    Gate.io pushes OHLCV changes every ~2s for the current candle, but open_time
    //    only changes when a new candle forms (~60s). We track both to keep the chart live.
    const auto it = m_lastKlineOpenTimes.find(symbol);
    const bool is_new_candle = (m_lastKlineOpenTimes.end() == it)
                            || (it->second != latest_kline->open_time);

    const auto close_it = m_lastKlineClose.find(symbol);
    const bool price_changed = (m_lastKlineClose.end() == close_it)
                            || (close_it->second != latest_kline->close);

    if (is_new_candle || price_changed)
    {
        // New candle or within-candle update — push the full snapshot.
        m_lastKlineOpenTimes[symbol] = latest_kline->open_time;
        m_lastKlineClose[symbol] = latest_kline->close;
        snap.kline.symbol = symbol;
        snap.kline.candles = kline_buf.snapshot(100);
    }
}

// ---------------------------------------------------------------------------
// pollMedium — 500 ms: positions + orders
// ---------------------------------------------------------------------------

void DashboardState::pollMedium(DashboardSnapshot &snap)
{
    // 1. Retrieve open positions and portfolio summary from the position manager.
    auto &pos_mgr = m_riskMgr.positionManager();
    snap.positions.positions = pos_mgr.getAllPositions();
    snap.positions.portfolio = pos_mgr.portfolioSummary();

    // 2. Retrieve active orders and recent execution reports from the order tracker.
    snap.orders.activeOrders = m_orderTracker.activeOrders();
    snap.orders.recentReports = m_orderTracker.recentReports(10);
}

// ---------------------------------------------------------------------------
// pollSlow — 1 s: strategies, risk, metrics
// ---------------------------------------------------------------------------

void DashboardState::pollSlow(DashboardSnapshot &snap)
{
    // 1. Retrieve strategy snapshots from the strategy manager.
    snap.strategies.strategies = m_strategyMgr.snapshot();

    // 2. Retrieve risk snapshot from the risk manager.
    snap.risk = m_riskMgr.riskSnapshot();

    // 3. Metrics are not yet implemented — mark as unavailable.
    snap.metrics.available = false;
}

// ---------------------------------------------------------------------------
// pollAi — 60 s check: AI analysis change detection
// ---------------------------------------------------------------------------

void DashboardState::pollAi(DashboardSnapshot &snap)
{
    // 1. Retrieve the latest AI analysis result (shared_ptr, may be nullptr).
    auto result = m_aiPipeline.lastResult();

    // 2. Compare the shared_ptr for change detection.
    //    The pointer changes only when a new analysis cycle completes.
    if (result.get() != m_lastAiResult.get())
    {
        m_lastAiResult = result;

        if (result)
        {
            snap.ai.available = true;
            snap.ai.result = *result;
            snap.ai.last_update_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        else
        {
            snap.ai.available = false;
        }
    }
}

// ---------------------------------------------------------------------------
// pollAccount — 10 s: exchange-reported account balance
// ---------------------------------------------------------------------------

void DashboardState::pollAccount(DashboardSnapshot &snap)
{
    // --- Futures account ---
    if (nullptr != m_restClient)
    {
        auto result = m_restClient->getFuturesAccountBalance();
        if (ok(result))
        {
            const auto &bal = value(result);
            snap.account.available         = true;
            snap.account.total             = bal.total;
            snap.account.available_balance = bal.available;
            snap.account.unrealised_pnl    = bal.unrealised_pnl;
            snap.account.position_margin   = bal.position_margin;
            snap.account.order_margin      = bal.order_margin;
            snap.account.currency          = bal.currency;
        }
        else
        {
            snap.account.available = false;
        }
    }
    else
    {
        snap.account.available = false;
    }

    // --- Spot account ---
    if (nullptr != m_spotRestClient)
    {
        auto spot_result = m_spotRestClient->getSpotAccounts();
        if (ok(spot_result))
        {
            const auto &arr = value(spot_result);
            for (const auto &item : arr)
            {
                if ("USDT" == item.value("currency", ""))
                {
                    snap.account.spot_available         = true;
                    snap.account.spot_available_balance = safeParseDouble(item.value("available", "0")).value_or(0.0);
                    double locked                       = safeParseDouble(item.value("locked", "0")).value_or(0.0);
                    snap.account.spot_total             = snap.account.spot_available_balance + locked;
                    snap.account.spot_currency          = "USDT";
                    break;
                }
            }
        }
        else
        {
            snap.account.spot_available = false;
        }
    }
    else
    {
        snap.account.spot_available = false;
    }
}

} // namespace pulse::webui
