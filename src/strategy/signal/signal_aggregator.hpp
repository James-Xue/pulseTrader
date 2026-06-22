#pragma once
// signal_aggregator.hpp — Multi-strategy weighted voting (Layer 6 Strategy Engine)
//
// Collects TradingSignals from multiple strategies and produces a single
// consolidated signal per symbol via weighted voting:
//
//   1. Each strategy's signal is weighted by its confidence score
//   2. Weights can be adjusted dynamically (future: by AI layer)
//   3. Aggregated confidence is compared against a threshold
//   4. If threshold is exceeded, a consolidated signal is emitted
//   5. Per-symbol cooldown prevents duplicate signals within a time window
//
// Thread safety:
//   - add_signal() is called from multiple strategy threads
//   - Uses std::mutex to protect the aggregation state
//   - evaluate_and_emit() is called from a dedicated evaluation thread or
//     inline from add_signal() when threshold is reached
//
// Usage:
//   SignalAggregator agg(config);
//   agg.set_output_callback([](const TradingSignal& s) { ... });
//   agg.set_weight("momentum_scalper_BTC_USDT", 1.0);
//   agg.add_signal(signal);  // from strategy thread

#include "core/config.hpp"
#include "strategy/signal_types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// SignalAggregator — weighted voting across multiple strategies
// ---------------------------------------------------------------------------
class SignalAggregator
{
  public:
    /// Callback type for consolidated output signals.
    using OutputCallback = std::function<void(const TradingSignal &)>;

    /// Construct with strategy configuration.
    ///
    /// Parameters:
    ///   1. config — StrategyConfig with threshold and cooldown settings
    explicit SignalAggregator(const StrategyConfig &config);

    /// Set the callback that receives consolidated signals.
    ///
    /// Typically wired to the order execution pipeline:
    ///   agg.set_output_callback([](const TradingSignal& s) { place_order(s); });
    void set_output_callback(OutputCallback cb);

    /// Set the voting weight for a specific strategy.
    ///
    /// Parameters:
    ///   1. strategy_id — unique ID of the strategy (from StrategyBase::id())
    ///   2. weight      — voting weight (default 1.0; AI layer can adjust)
    void set_weight(const std::string &strategy_id, double weight);

    /// Add a signal from a strategy (called from strategy threads).
    ///
    /// The signal is accumulated per-symbol. When the aggregated confidence
    /// exceeds the threshold and cooldown has elapsed, a consolidated signal
    /// is emitted via the output callback.
    ///
    /// Parameters:
    ///   1. signal — the trading signal to aggregate
    void add_signal(const TradingSignal &signal);

    /// Get the current aggregated confidence for a symbol.
    ///
    /// Returns the sum of weighted confidences for the dominant direction.
    [[nodiscard]] double aggregated_confidence(const Symbol &symbol) const;

    /// Get the number of signals received (for monitoring).
    [[nodiscard]] std::uint64_t signal_count() const;

    /// Reset all aggregation state (for testing or session restart).
    void reset();

  private:
    /// Per-symbol aggregation state.
    struct SymbolState
    {
        double buy_weighted_sum = 0.0;   ///< Sum of weighted Buy confidences.
        double sell_weighted_sum = 0.0;  ///< Sum of weighted Sell confidences.
        double weight_sum = 0.0;         ///< Sum of all weights (for normalization).
        std::int64_t last_signal_ms = 0; ///< Timestamp of last emitted signal (ms).
        Price last_price = 0.0;          ///< Latest reference price.
        MarketType market_type = MarketType::Spot; ///< Market type from input signals.
    };

    StrategyConfig config_;
    OutputCallback output_callback_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, double> weights_;       ///< Per-strategy weights.
    std::unordered_map<Symbol, SymbolState> symbol_states_; ///< Per-symbol aggregation.
    std::uint64_t signal_count_{ 0 };                       ///< Total signals received.

    /// Evaluate whether a consolidated signal should be emitted for a symbol.
    ///
    /// Called from add_signal() after accumulating a new signal.
    /// Checks threshold and cooldown before emitting.
    void evaluate_and_emit(const Symbol &symbol);

    /// Get the weight for a strategy (default 1.0 if not set).
    [[nodiscard]] double get_weight(const std::string &strategy_id) const;

    /// Get current time in milliseconds since epoch.
    [[nodiscard]] static std::int64_t now_ms();
};

} // namespace pulse::strategy
