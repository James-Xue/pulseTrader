// signal_aggregator.cpp — Multi-strategy weighted voting (Layer 6 Strategy Engine)

#include "pulse/strategy/signal/signal_aggregator.hpp"

#include "pulse/logging/logger.hpp"

#include <algorithm>
#include <chrono>

namespace pulse::strategy
{

SignalAggregator::SignalAggregator(const StrategyConfig &config)
    : config_{ config }
{
}

void SignalAggregator::set_output_callback(OutputCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    output_callback_ = std::move(cb);
}

void SignalAggregator::set_weight(const std::string &strategy_id, double weight)
{
    std::lock_guard<std::mutex> lock(mutex_);
    weights_[strategy_id] = weight;
}

void SignalAggregator::add_signal(const TradingSignal &signal)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Ignore Flat signals — they carry no directional conviction.
    if (SignalType::Flat == signal.type)
    {
        return;
    }

    ++signal_count_;

    // 2. Look up the weight for this strategy.
    const double weight = get_weight(signal.strategy_id);

    // 3. Accumulate into the per-symbol state.
    auto &state = symbol_states_[signal.symbol];
    const double weighted_confidence = signal.confidence * weight;

    if (SignalType::Buy == signal.type)
    {
        state.buy_weighted_sum += weighted_confidence;
    }
    else
    {
        state.sell_weighted_sum += weighted_confidence;
    }
    state.weight_sum += weight;
    state.last_price = signal.price;

    // 4. Evaluate whether we should emit a consolidated signal.
    evaluate_and_emit(signal.symbol);
}

double SignalAggregator::aggregated_confidence(const Symbol &symbol) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = symbol_states_.find(symbol);
    if (symbol_states_.end() == it)
    {
        return 0.0;
    }

    const auto &state = it->second;
    return std::max(state.buy_weighted_sum, state.sell_weighted_sum);
}

std::uint64_t SignalAggregator::signal_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return signal_count_;
}

void SignalAggregator::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    symbol_states_.clear();
    signal_count_ = 0;
}

// ---------------------------------------------------------------------------
// Private methods
// ---------------------------------------------------------------------------

void SignalAggregator::evaluate_and_emit(const Symbol &symbol)
{
    // Note: caller must already hold mutex_.

    auto &state = symbol_states_[symbol];

    // 1. Determine dominant direction.
    const bool buy_dominant = state.buy_weighted_sum >= state.sell_weighted_sum;
    const double dominant_sum = buy_dominant ? state.buy_weighted_sum : state.sell_weighted_sum;

    // 2. Normalize by weight sum (if any weights registered).
    double normalized_confidence = dominant_sum;
    if (state.weight_sum > 0.0)
    {
        normalized_confidence = dominant_sum / state.weight_sum;
    }

    // 3. Check threshold.
    if (normalized_confidence < config_.signal_aggregator_threshold)
    {
        return;
    }

    // 4. Check cooldown.
    const auto current_ms = now_ms();
    const auto cooldown_ms = static_cast<std::int64_t>(config_.signal_cooldown_sec) * 1000;
    if (current_ms - state.last_signal_ms < cooldown_ms)
    {
        return;
    }

    // 5. Build consolidated signal.
    TradingSignal consolidated;
    consolidated.type = buy_dominant ? SignalType::Buy : SignalType::Sell;
    consolidated.symbol = symbol;
    consolidated.confidence = std::clamp(normalized_confidence, 0.0, 1.0);
    consolidated.price = state.last_price;
    consolidated.strategy_id = "signal_aggregator";
    consolidated.timestamp = now();
    consolidated.reason = "Aggregated signal: "
        + std::to_string(state.buy_weighted_sum) + " buy vs "
        + std::to_string(state.sell_weighted_sum) + " sell (threshold="
        + std::to_string(config_.signal_aggregator_threshold) + ")";

    PULSE_LOG_INFO("strategy", "[aggregator] {} {} confidence={:.4f} for {}",
        symbol, buy_dominant ? "BUY" : "SELL", normalized_confidence, symbol);

    // 6. Emit via callback.
    if (nullptr != output_callback_)
    {
        output_callback_(consolidated);
    }

    // 7. Reset per-symbol accumulation and record emission time.
    state.buy_weighted_sum = 0.0;
    state.sell_weighted_sum = 0.0;
    state.weight_sum = 0.0;
    state.last_signal_ms = current_ms;
}

double SignalAggregator::get_weight(const std::string &strategy_id) const
{
    // Note: caller must already hold mutex_.
    auto it = weights_.find(strategy_id);
    if (weights_.end() != it)
    {
        return it->second;
    }
    return 1.0; // Default weight.
}

std::int64_t SignalAggregator::now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace pulse::strategy
