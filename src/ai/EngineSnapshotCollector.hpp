#pragma once
// engine_snapshot_collector.hpp — real market + performance snapshot for the AI cycle
//
// Replaces the heartbeat's hard-coded BTC_USDT stub: collects the primary
// symbol's ticker + klines from the live feeds, one-line tickers for every
// traded symbol, and per-strategy recent performance from the trades DB
// (when SQLite is enabled and a recorder is wired). collect() never throws —
// every source degrades to an empty field on failure so the AI cycle keeps
// running with less context.

#include "ai/PipelineContext.hpp"
#include "market/MarketFeed.hpp"
#include "strategy/StrategyHandle.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::trade_recorder
{
class TradeRecorder;
}

namespace pulse::ai
{

class EngineSnapshotCollector
{
  public:
    /// Construct the collector.
    ///
    /// Parameters:
    ///   1..3. feeds — spot / futures / cfd MarketFeed pointers (nullable).
    ///   4. strategies — strategy handles (identity + symbol for feed lookup).
    ///   5. recorder — trade recorder for performance stats (nullable;
    ///      absent without PULSE_ENABLE_SQLITE or when the DB is off).
    ///   6. stats_lookback_ns — performance window (0 = skip stats).
    EngineSnapshotCollector(market::MarketFeed *spot_feed,
                            market::MarketFeed *futures_feed,
                            market::MarketFeed *cfd_feed,
                            std::vector<strategy::StrategyHandle> strategies,
                            trade_recorder::TradeRecorder *recorder,
                            std::int64_t stats_lookback_ns);

    /// Collect the current pipeline context. Never throws.
    [[nodiscard]] PipelineContext collect() const;

    /// Pure, test-friendly assembly from explicit sources — fills the market
    /// snapshot from a ticker cache + kline accessor (both nullable).
    [[nodiscard]] static PipelineContext fromSources(
        market::TickerCache *cache,
        const std::function<std::vector<market::Kline>(const std::string &)> &klineFn,
        const std::vector<strategy::StrategyHandle> &handles,
        std::vector<StrategyPerformance> performance);

  private:
    [[nodiscard]] market::MarketFeed *feedFor(const strategy::StrategyHandle &h) const;
    [[nodiscard]] std::vector<StrategyPerformance> collectPerformance() const;

    market::MarketFeed *m_spotFeed;
    market::MarketFeed *m_futuresFeed;
    market::MarketFeed *m_cfdFeed;
    std::vector<strategy::StrategyHandle> m_strategies;
    trade_recorder::TradeRecorder *m_recorder; ///< Nullable.
    std::int64_t m_statsLookbackNs;
};

} // namespace pulse::ai
