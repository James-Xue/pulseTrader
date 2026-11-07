#pragma once
// SignalBoard.hpp — In-memory factor board for strategy signals (Layer 6)
//
// Every strategy signal (and the aggregator's consolidated output) is
// published here so the control plane (`get_signals`) and external
// consumers (the XAUUSD sub-agent) can read what the strategies currently
// "think" without the engine placing any order. Purely observational —
// the board never triggers execution.
//
// Retention: the latest entry per source (strategy_id) — publishing
// overwrites. Consumers filter by publish-time freshness (no auto-prune).

#include "strategy/signal_types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>

namespace pulse::strategy
{

class SignalBoard
{
  public:
    /// A board entry: the signal plus its publish time (engine clock, ms).
    struct Entry
    {
        TradingSignal signal;
        std::int64_t ts_ms{ 0 };
    };

    /// Construct with the aggregator threshold (echoed inside the aggregate
    /// entry so consumers can compare consensus confidence against it).
    explicit SignalBoard(double aggregate_threshold);

    /// Publish (overwrite) the latest signal for a strategy.
    /// Called from strategy threads (once per poll cycle at most).
    void publish(const TradingSignal &signal);

    /// Publish the aggregator's consolidated output signal.
    void publishAggregate(const TradingSignal &signal);

    /// Full snapshot: per-source entries + aggregate entry (absent until the
    /// first consolidated signal). Timestamps are raw epoch ms — the control
    /// plane adds human-readable *_str fields with its display timezone.
    [[nodiscard]] nlohmann::json snapshot() const;

    /// Number of per-source entries currently held.
    [[nodiscard]] std::uint64_t entryCount() const;

  private:
    [[nodiscard]] static std::int64_t nowMs();

    mutable std::shared_mutex m_mutex;
    std::map<std::string, Entry> m_latest; ///< key = strategy_id.
    std::optional<Entry> m_aggregate;
    double m_aggregateThreshold;
};

} // namespace pulse::strategy
