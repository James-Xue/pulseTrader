// dashboard_state.cpp — Tiered polling aggregator implementation (Layer 9 WebUI)
//
// Implements the poll_loop that dispatches to tiered poll methods at different
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
                               ai::AiPipeline &ai_pipeline)
    : config_{ config }
    , market_feed_{ market_feed }
    , strategy_mgr_{ strategy_mgr }
    , risk_mgr_{ risk_mgr }
    , order_tracker_{ order_tracker }
    , ai_pipeline_{ ai_pipeline }
{
}

DashboardState::~DashboardState()
{
    stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void DashboardState::set_snapshot_callback(SnapshotCallback cb)
{
    snapshot_callback_ = std::move(cb);
}

void DashboardState::start()
{
    // Guard: do not start if already running.
    if (running_.load(std::memory_order_acquire))
    {
        return;
    }

    poll_thread_ = std::jthread([this](std::stop_token stoken)
    {
        poll_loop(stoken);
    });
}

void DashboardState::stop()
{
    // Request cooperative cancellation.
    poll_thread_.request_stop();

    // Wait for the thread to finish (jthread joins on destruction, but we
    // want stop() to block until the thread has exited).
    if (poll_thread_.joinable())
    {
        poll_thread_.join();
    }
}

[[nodiscard]] std::shared_ptr<const DashboardSnapshot> DashboardState::latest() const
{
    std::shared_lock lock{ snapshot_mutex_ };
    return latest_snapshot_;
}

[[nodiscard]] bool DashboardState::running() const
{
    return running_.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Poll loop
// ---------------------------------------------------------------------------

void DashboardState::poll_loop(std::stop_token stoken)
{
    using Clock = std::chrono::steady_clock;

    // Mark the thread as running.
    running_.store(true, std::memory_order_release);

    // Track last-poll times for each tier.
    auto last_fast   = Clock::now();
    auto last_medium = Clock::now();
    auto last_slow   = Clock::now();
    auto last_ai     = Clock::now();

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
            poll_fast(snap);
            poll_klines(snap);
            last_fast = now;
            updated = true;
        }

        // 3. Medium tier: 500 ms — positions + orders.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_medium).count() >= 500)
        {
            poll_medium(snap);
            last_medium = now;
            updated = true;
        }

        // 4. Slow tier: 1 s — strategies, risk, metrics.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_slow).count() >= 1000)
        {
            poll_slow(snap);
            last_slow = now;
            updated = true;
        }

        // 5. AI tier: 60 s — AI analysis change detection.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ai).count() >= 60000)
        {
            poll_ai(snap);
            last_ai = now;
            updated = true;
        }

        // Also poll AI on the very first iteration so the dashboard has
        // an initial AI snapshot even before the 60 s interval fires.
        if (0 == snap.timestamp_ms)
        {
            poll_ai(snap);
        }

        // 6. If any tier updated, stamp the snapshot and publish it.
        if (updated)
        {
            snap.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            // Create an immutable shared_ptr snapshot for readers.
            auto published = std::make_shared<const DashboardSnapshot>(snap);

            {
                std::unique_lock lock{ snapshot_mutex_ };
                latest_snapshot_ = published;
            }

            // Invoke the callback (if set) outside the lock.
            if (snapshot_callback_)
            {
                snapshot_callback_(published);
            }
        }
    }

    // Mark the thread as no longer running.
    running_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// poll_fast — 200 ms: order books + ticker prices
// ---------------------------------------------------------------------------

void DashboardState::poll_fast(DashboardSnapshot &snap)
{
    // 1. Get all tracked symbols from the ticker cache.
    const auto symbols = market_feed_.ticker_cache().symbols();

    // 2. For the first symbol (primary), retrieve top 20 bids/asks.
    //    The dashboard currently shows one symbol's order book at a time.
    if (!symbols.empty())
    {
        const auto &symbol = symbols[0];
        auto &ob_mgr = market_feed_.orderbook_manager();

        snap.order_book.symbol = symbol;
        snap.order_book.bids = ob_mgr.top_bids(symbol, 20);
        snap.order_book.asks = ob_mgr.top_asks(symbol, 20);

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
// poll_klines — per candle change detection
// ---------------------------------------------------------------------------

void DashboardState::poll_klines(DashboardSnapshot &snap)
{
    // 1. Get all tracked symbols.
    const auto symbols = market_feed_.ticker_cache().symbols();

    if (symbols.empty())
    {
        return;
    }

    // 2. For the primary symbol, check if a new candle has formed.
    const auto &symbol = symbols[0];

    auto &kline_buf = market_feed_.get_kline_buffer(symbol);
    const auto latest_kline = kline_buf.latest();

    if (!latest_kline.has_value())
    {
        return;
    }

    // 3. Compare open_time and close price to detect new candles or within-candle updates.
    //    Gate.io pushes OHLCV changes every ~2s for the current candle, but open_time
    //    only changes when a new candle forms (~60s). We track both to keep the chart live.
    const auto it = last_kline_open_times_.find(symbol);
    const bool is_new_candle = (last_kline_open_times_.end() == it)
                            || (it->second != latest_kline->open_time);

    const auto close_it = last_kline_close_.find(symbol);
    const bool price_changed = (last_kline_close_.end() == close_it)
                            || (close_it->second != latest_kline->close);

    if (is_new_candle || price_changed)
    {
        // New candle or within-candle update — push the full snapshot.
        last_kline_open_times_[symbol] = latest_kline->open_time;
        last_kline_close_[symbol] = latest_kline->close;
        snap.kline.symbol = symbol;
        snap.kline.candles = kline_buf.snapshot(100);
    }
}

// ---------------------------------------------------------------------------
// poll_medium — 500 ms: positions + orders
// ---------------------------------------------------------------------------

void DashboardState::poll_medium(DashboardSnapshot &snap)
{
    // 1. Retrieve open positions and portfolio summary from the position manager.
    auto &pos_mgr = risk_mgr_.position_manager();
    snap.positions.positions = pos_mgr.get_all_positions();
    snap.positions.portfolio = pos_mgr.portfolio_summary();

    // 2. Retrieve active orders and recent execution reports from the order tracker.
    snap.orders.active_orders = order_tracker_.active_orders();
    snap.orders.recent_reports = order_tracker_.recent_reports(10);
}

// ---------------------------------------------------------------------------
// poll_slow — 1 s: strategies, risk, metrics
// ---------------------------------------------------------------------------

void DashboardState::poll_slow(DashboardSnapshot &snap)
{
    // 1. Retrieve strategy snapshots from the strategy manager.
    snap.strategies.strategies = strategy_mgr_.snapshot();

    // 2. Retrieve risk snapshot from the risk manager.
    snap.risk = risk_mgr_.risk_snapshot();

    // 3. Metrics are not yet implemented — mark as unavailable.
    snap.metrics.available = false;
}

// ---------------------------------------------------------------------------
// poll_ai — 60 s check: AI analysis change detection
// ---------------------------------------------------------------------------

void DashboardState::poll_ai(DashboardSnapshot &snap)
{
    // 1. Retrieve the latest AI analysis result (shared_ptr, may be nullptr).
    auto result = ai_pipeline_.last_result();

    // 2. Compare the shared_ptr for change detection.
    //    The pointer changes only when a new analysis cycle completes.
    if (result.get() != last_ai_result_.get())
    {
        last_ai_result_ = result;

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

} // namespace pulse::webui
