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
    return m_context;
}

StrategyContext &StrategyBase::context()
{
    return m_context;
}

void StrategyBase::setSignalCallback(SignalCallback cb)
{
    m_signalCallback = std::move(cb);
}

std::atomic<bool> &StrategyBase::active()
{
    return m_active;
}

void StrategyBase::emitSignal(const TradingSignal &signal)
{
    // 1. Drop if no callback is registered.
    if (nullptr == m_signalCallback)
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
    enriched.market_type = m_context.config.market_type;

    // 4. Forward to the callback (typically StrategyManager → SignalAggregator).
    m_signalCallback(enriched);
}

// ---------------------------------------------------------------------------
// StrategyManager — lifecycle
// ---------------------------------------------------------------------------

StrategyManager::~StrategyManager()
{
    // Ensure threads are stopped on destruction.
    stop();
}

void StrategyManager::registerStrategy(std::unique_ptr<StrategyBase> strategy)
{
    m_strategies.push_back(std::move(strategy));
}

void StrategyManager::setSignalCallback(SignalCallback cb)
{
    m_signalCallback = std::move(cb);
}

void StrategyManager::start()
{
    // 1. Wire the signal callback into each registered strategy.
    for (auto &strategy : m_strategies)
    {
        if (!strategy->context().config.enabled)
        {
            PULSE_LOG_INFO("strategy", "Skipping disabled strategy: {}", strategy->name());
            continue;
        }

        strategy->setSignalCallback(m_signalCallback);
        strategy->active().store(true, std::memory_order_release);

        // 2. Spawn a std::jthread for this strategy.
        //    std::jthread automatically joins on destruction.
        m_threads.emplace_back([this, &s = *strategy](std::stop_token stoken)
            {
                strategyLoop(s, stoken);
            });

        PULSE_LOG_INFO("strategy", "Started strategy: {} on {}",
            strategy->name(), strategy->context().config.symbol);
    }
}

void StrategyManager::stop()
{
    // 1. Signal all strategies to stop.
    for (auto &strategy : m_strategies)
    {
        strategy->active().store(false, std::memory_order_release);
    }

    // 2. Request stop on all jthreads (triggers stop_token).
    for (auto &t : m_threads)
    {
        t.request_stop();
    }

    // 3. jthreads auto-join on destruction, but we clear the vector explicitly.
    m_threads.clear();

    PULSE_LOG_INFO("strategy", "All strategy threads stopped ({} strategies)", m_strategies.size());
}

std::size_t StrategyManager::strategyCount() const
{
    return m_strategies.size();
}

std::size_t StrategyManager::runningCount() const
{
    std::size_t count = 0;
    for (const auto &strategy : m_strategies)
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
    result.reserve(m_strategies.size());
    for (const auto &s : m_strategies)
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

std::vector<StrategyParams *> StrategyManager::allParams()
{
    std::vector<StrategyParams *> result;
    result.reserve(m_strategies.size());
    for (const auto &s : m_strategies)
    {
        result.push_back(&s->params());
    }
    return result;
}

// ---------------------------------------------------------------------------
// strategyLoop — the main loop for a single strategy thread
// ---------------------------------------------------------------------------
void StrategyManager::strategyLoop(StrategyBase &strategy, std::stop_token stoken)
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

        // 1. Poll ticker for onTick().
        auto ticker_opt = feed->tickerCache().get(cfg.symbol);
        if (ticker_opt.has_value())
        {
            strategy.onTick(ticker_opt.value());
        }

        // 2. Check for new closed kline → onKline().
        auto &kline_buf = feed->getKlineBuffer(cfg.symbol);
        auto latest_kline = kline_buf.latest();
        if (latest_kline.has_value() && latest_kline->closed
            && latest_kline->open_time != last_kline_time)
        {
            last_kline_time = latest_kline->open_time;
            strategy.onKline(latest_kline.value());
        }

        // 3. Poll order book for onOrderbook().
        auto book_opt = feed->orderbookManager().get(cfg.symbol);
        if (book_opt.has_value())
        {
            strategy.onOrderbook(book_opt.value());
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
