#pragma once
// pipeline_context.hpp — aggregated AI-cycle input (market + performance)
//
// One snapshot object feeds the whole AI cycle: the primary symbol's market
// data, one-line tickers for every traded symbol, and per-strategy recent
// performance (from the trades DB). Produced by EngineSnapshotCollector and
// consumed by PromptBuilder. All fields degrade to empty on any source
// failure — the cycle keeps running with less context rather than dying.

#include "market/KlineBuffer.hpp"
#include "market/TickerCache.hpp"

#include <functional>
#include <string>
#include <vector>

namespace pulse::ai
{

/// Lightweight aggregate of market data for prompt assembly — bundles the
/// ticker (latest price, bid/ask, volume) with recent K-line candles.
struct MarketSnapshot
{
    market::Ticker ticker;             ///< Latest ticker for the symbol.
    std::vector<market::Kline> klines; ///< Last N closed candles (chronological).
};

/// Per-strategy recent performance (AI feedback signal).
struct StrategyPerformance
{
    std::string strategy_name;
    std::string market_type;
    std::int64_t trade_count{ 0 };
    double total_pnl{ 0.0 };
    double win_rate{ 0.0 };
    double total_fees{ 0.0 };
};

/// Aggregated context for one AI analysis cycle.
struct PipelineContext
{
    MarketSnapshot market;                  ///< Primary symbol ticker + klines.
    std::vector<market::Ticker> symbol_tickers; ///< One line per traded symbol.
    std::vector<StrategyPerformance> performance;
};

/// Snapshot provider hook — HeartbeatScheduler calls this each beat.
using SnapshotProvider = std::function<PipelineContext()>;

} // namespace pulse::ai
