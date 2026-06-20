// strategy_manager.cpp — Multi-strategy orchestration (Layer 6 Strategy Engine)

#include "strategy/strategy_manager.hpp"

#include "logging/logger.hpp"

#include <algorithm>
#include <chrono>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// StrategyBase — non-virtual method implementations
// ---------------------------------------------------------------------------

const StrategyContext &StrategyBase::context() const
{
    return context_;
}

StrategyContext &StrategyBase::context()
{
    return context_;
}

void StrategyBase::set_signal_callback(SignalCallback cb)
{
    signal_callback_ = std::move(cb);
}

std::atomic<bool> &StrategyBase::active()
{
    return active_;
}

void StrategyBase::emit_signal(const TradingSignal &signal)
{
    // 1. Drop if no callback is registered.
    if (nullptr == signal_callback_)
    {
        return;
    }

    // 2. Drop if confidence is below the strategy's minimum threshold.
    const double min_conf = params().min_confidence.load(std::memory_order_acquire);
    if (signal.confidence < min_conf)
    {
        return;
    }

    // 3. Ensure market_type is set from strategy config (allows downstream routing).
    TradingSignal enriched = signal;
    enriched.market_type = context_.config.market_type;

    // 4. Forward to the callback (typically StrategyManager → SignalAggregator).
    signal_callback_(enriched);
}

// ---------------------------------------------------------------------------
// StrategyManager — lifecycle
// ---------------------------------------------------------------------------

StrategyManager::~StrategyManager()
{
    // Ensure threads are stopped on destruction.
    stop();
}

void StrategyManager::register_strategy(std::unique_ptr<StrategyBase> strategy)
{
    strategies_.push_back(std::move(strategy));
}

void StrategyManager::set_signal_callback(SignalCallback cb)
{
    signal_callback_ = std::move(cb);
}

void StrategyManager::start()
{
    // 1. Wire the signal callback into each registered strategy.
    for (auto &strategy : strategies_)
    {
        if (!strategy->context().config.enabled)
        {
            PULSE_LOG_INFO("strategy", "Skipping disabled strategy: {}", strategy->name());
            continue;
        }

        strategy->set_signal_callback(signal_callback_);
        strategy->active().store(true, std::memory_order_release);

        // 2. Spawn a std::jthread for this strategy.
        //    std::jthread automatically joins on destruction.
        threads_.emplace_back([this, &s = *strategy](std::stop_token stoken)
            {
                strategy_loop(s, stoken);
            });

        PULSE_LOG_INFO("strategy", "Started strategy: {} on {}",
            strategy->name(), strategy->context().config.symbol);
    }
}

void StrategyManager::stop()
{
    // 1. Signal all strategies to stop.
    for (auto &strategy : strategies_)
    {
        strategy->active().store(false, std::memory_order_release);
    }

    // 2. Request stop on all jthreads (triggers stop_token).
    for (auto &t : threads_)
    {
        t.request_stop();
    }

    // 3. jthreads auto-join on destruction, but we clear the vector explicitly.
    threads_.clear();

    PULSE_LOG_INFO("strategy", "All strategy threads stopped ({} strategies)", strategies_.size());
}

std::size_t StrategyManager::strategy_count() const
{
    return strategies_.size();
}

std::size_t StrategyManager::running_count() const
{
    std::size_t count = 0;
    for (const auto &strategy : strategies_)
    {
        if (strategy->active().load(std::memory_order_acquire))
        {
            ++count;
        }
    }
    return count;
}

std::vector<StrategySnapshot> StrategyManager::snapshot() const
{
    std::vector<StrategySnapshot> result;
    result.reserve(strategies_.size());
    for (const auto &s : strategies_)
    {
        StrategySnapshot snap;
        snap.name = s->name();
        snap.id = s->id();
        snap.symbol = s->context().config.symbol;
        snap.enabled = s->context().config.enabled;
        snap.running = s->active().load(std::memory_order_acquire);
        snap.poll_interval_ms = s->context().config.poll_interval_ms;
        result.push_back(std::move(snap));
    }
    return result;
}

std::vector<StrategyParams *> StrategyManager::all_params()
{
    std::vector<StrategyParams *> result;
    result.reserve(strategies_.size());
    for (const auto &s : strategies_)
    {
        result.push_back(&s->params());
    }
    return result;
}

// ---------------------------------------------------------------------------
// strategy_loop — the main loop for a single strategy thread
// ---------------------------------------------------------------------------
void StrategyManager::strategy_loop(StrategyBase &strategy, std::stop_token stoken)
{
    const auto &cfg = strategy.context().config;
    const auto poll_interval = std::chrono::milliseconds(cfg.poll_interval_ms);

    // Track last-seen kline to detect new closed candles.
    std::int64_t last_kline_time = 0;

    PULSE_LOG_INFO("strategy", "[{}] Thread started, polling every {}ms",
        strategy.name(), cfg.poll_interval_ms);

    while (!stoken.stop_requested() && strategy.active().load(std::memory_order_acquire))
    {
        auto *feed = strategy.context().market_feed;
        if (nullptr == feed)
        {
            PULSE_LOG_ERROR("strategy", "[{}] No market feed — stopping", strategy.name());
            break;
        }

        // 1. Poll ticker for on_tick().
        auto ticker_opt = feed->ticker_cache().get(cfg.symbol);
        if (ticker_opt.has_value())
        {
            strategy.on_tick(ticker_opt.value());
        }

        // 2. Check for new closed kline → on_kline().
        auto &kline_buf = feed->get_kline_buffer(cfg.symbol);
        auto latest_kline = kline_buf.latest();
        if (latest_kline.has_value() && latest_kline->closed
            && latest_kline->open_time != last_kline_time)
        {
            last_kline_time = latest_kline->open_time;
            strategy.on_kline(latest_kline.value());
        }

        // 3. Poll order book for on_orderbook().
        auto book_opt = feed->orderbook_manager().get(cfg.symbol);
        if (book_opt.has_value())
        {
            strategy.on_orderbook(book_opt.value());
        }

        // 4. Sleep until next poll, but wake early if stop is requested.
        if (stoken.stop_requested())
        {
            break;
        }

        std::this_thread::sleep_for(poll_interval);
    }

    PULSE_LOG_INFO("strategy", "[{}] Thread exiting", strategy.name());
}

} // namespace pulse::strategy
