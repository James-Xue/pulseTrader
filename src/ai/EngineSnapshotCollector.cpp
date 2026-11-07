// engine_snapshot_collector.cpp — real market + performance snapshot

#include "ai/EngineSnapshotCollector.hpp"

#include "logging/Logger.hpp"

#ifdef PULSE_ENABLE_SQLITE
#include "trade_recorder/TradeRecorder.hpp"
#endif

#include <algorithm>
#include <chrono>

namespace pulse::ai
{

namespace
{

constexpr std::size_t kKlinesPerSymbol = 10;

} // namespace

EngineSnapshotCollector::EngineSnapshotCollector(
    market::MarketFeed *spot_feed,
    market::MarketFeed *futures_feed,
    market::MarketFeed *cfd_feed,
    std::vector<strategy::StrategyHandle> strategies,
    trade_recorder::TradeRecorder *recorder,
    std::int64_t stats_lookback_ns)
    : m_spotFeed{ spot_feed }
    , m_futuresFeed{ futures_feed }
    , m_cfdFeed{ cfd_feed }
    , m_strategies{ std::move(strategies) }
    , m_recorder{ recorder }
    , m_statsLookbackNs{ stats_lookback_ns }
{
}

market::MarketFeed *EngineSnapshotCollector::feedFor(
    const strategy::StrategyHandle &h) const
{
    if ("futures" == h.market_type)
    {
        return m_futuresFeed;
    }
    if ("cfd" == h.market_type)
    {
        return m_cfdFeed;
    }
    return m_spotFeed;
}

std::vector<StrategyPerformance>
EngineSnapshotCollector::collectPerformance() const
{
#ifndef PULSE_ENABLE_SQLITE
    return {}; // No trades DB in this build — no performance feedback.
#else
    if (nullptr == m_recorder || m_statsLookbackNs <= 0)
    {
        return {};
    }
    try
    {
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto summary = m_recorder->getStrategySummary(
            now_ns - m_statsLookbackNs, now_ns);
        if (!ok(summary))
        {
            PULSE_LOG_WARN("ai", "snapshot: strategy summary failed: {}",
                           error(summary).message);
            return {};
        }
        std::vector<StrategyPerformance> result;
        for (const auto &row : value(summary))
        {
            result.push_back({ row.strategy_name, row.market_type,
                               row.trade_count, row.total_pnl,
                               row.win_rate, row.total_fees });
        }
        return result;
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_WARN("ai", "snapshot: strategy summary threw: {}", e.what());
        return {};
    }
#endif
}

PipelineContext EngineSnapshotCollector::collect() const
{
    try
    {
        // Primary market = first handle's symbol, looked up in its own feed.
        PipelineContext ctx;
        if (!m_strategies.empty())
        {
            const auto &primary = m_strategies.front();
            auto *feed = feedFor(primary);
            if (nullptr != feed)
            {
                const auto ticker = feed->tickerCache().get(primary.symbol);
                if (ticker.has_value())
                {
                    ctx.market.ticker = ticker.value();
                }
                ctx.market.klines = feed->getKlineBuffer(primary.symbol)
                                        .snapshot(kKlinesPerSymbol);
            }

            // One-line tickers for every traded symbol (deduplicated).
            std::vector<std::string> seen;
            for (const auto &h : m_strategies)
            {
                if (std::find(seen.begin(), seen.end(), h.symbol) != seen.end())
                {
                    continue;
                }
                seen.push_back(h.symbol);
                auto *f = feedFor(h);
                if (nullptr == f)
                {
                    continue;
                }
                const auto t = f->tickerCache().get(h.symbol);
                if (t.has_value())
                {
                    ctx.symbol_tickers.push_back(t.value());
                }
            }
        }

        ctx.performance = collectPerformance();
        return ctx;
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_WARN("ai", "snapshot: collect failed — degraded cycle: {}",
                       e.what());
        return PipelineContext{};
    }
}

PipelineContext EngineSnapshotCollector::fromSources(
    market::TickerCache *cache,
    const std::function<std::vector<market::Kline>(const std::string &)> &klineFn,
    const std::vector<strategy::StrategyHandle> &handles,
    std::vector<StrategyPerformance> performance)
{
    PipelineContext ctx;
    ctx.performance = std::move(performance);
    if (handles.empty())
    {
        return ctx;
    }

    if (nullptr != cache)
    {
        const auto ticker = cache->get(handles.front().symbol);
        if (ticker.has_value())
        {
            ctx.market.ticker = ticker.value();
        }
    }
    if (klineFn)
    {
        ctx.market.klines = klineFn(handles.front().symbol);
    }

    std::vector<std::string> seen;
    for (const auto &h : handles)
    {
        if (std::find(seen.begin(), seen.end(), h.symbol) != seen.end())
        {
            continue;
        }
        seen.push_back(h.symbol);
        if (nullptr != cache)
        {
            const auto t = cache->get(h.symbol);
            if (t.has_value())
            {
                ctx.symbol_tickers.push_back(t.value());
            }
        }
    }
    return ctx;
}

} // namespace pulse::ai
