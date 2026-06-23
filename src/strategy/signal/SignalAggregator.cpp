// signal_aggregator.cpp — Multi-strategy weighted voting (Layer 6 Strategy Engine)

#include "strategy/signal/SignalAggregator.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <chrono>

namespace pulse::strategy
{

SignalAggregator::SignalAggregator(const StrategyConfig &config)
    : m_config{ config }
{
}

void SignalAggregator::setOutputCallback(OutputCallback cb)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outputCallback = std::move(cb);
}

void SignalAggregator::setWeight(const std::string &strategy_id, double weight)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_weights[strategy_id] = weight;
}

void SignalAggregator::addSignal(const TradingSignal &signal)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // 1. Ignore Flat signals — they carry no directional conviction.
    if (SignalType::Flat == signal.type)
    {
        return;
    }

    ++m_signalCount;

    // 2. Look up the weight for this strategy.
    const double weight = getWeight(signal.strategy_id);

    // 3. Accumulate into the per-symbol state.
    auto &state = m_symbolStates[signal.symbol];
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
    state.market_type = signal.market_type;

    // 4. Evaluate whether we should emit a consolidated signal.
    evaluateAndEmit(signal.symbol);
}

double SignalAggregator::aggregatedConfidence(const Symbol &symbol) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_symbolStates.find(symbol);
    if (m_symbolStates.end() == it)
    {
        return 0.0;
    }

    const auto &state = it->second;
    return std::max(state.buy_weighted_sum, state.sell_weighted_sum);
}

std::uint64_t SignalAggregator::signalCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_signalCount;
}

void SignalAggregator::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_symbolStates.clear();
    m_signalCount = 0;
}

// ---------------------------------------------------------------------------
// Private methods
// ---------------------------------------------------------------------------

void SignalAggregator::evaluateAndEmit(const Symbol &symbol)
{
    // Note: caller must already hold m_mutex.

    auto &state = m_symbolStates[symbol];

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
    if (normalized_confidence < m_config.signal_aggregator_threshold)
    {
        return;
    }

    // 4. Check cooldown.
    const auto current_ms = nowMs();
    const auto cooldown_ms = static_cast<std::int64_t>(m_config.signal_cooldown_sec) * 1000;
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
    consolidated.market_type = state.market_type;
    consolidated.strategy_id = "signal_aggregator";
    consolidated.timestamp = now();
    consolidated.reason = "Aggregated signal: "
        + std::to_string(state.buy_weighted_sum) + " buy vs "
        + std::to_string(state.sell_weighted_sum) + " sell (threshold="
        + std::to_string(m_config.signal_aggregator_threshold) + ")";

    PULSE_LOG_INFO("strategy", "[aggregator] {} {} confidence={:.4f} for {}",
        symbol, buy_dominant ? "BUY" : "SELL", normalized_confidence, symbol);

    // 6. Emit via callback.
    if (nullptr != m_outputCallback)
    {
        m_outputCallback(consolidated);
    }

    // 7. Reset per-symbol accumulation and record emission time.
    state.buy_weighted_sum = 0.0;
    state.sell_weighted_sum = 0.0;
    state.weight_sum = 0.0;
    state.last_signal_ms = current_ms;
}

double SignalAggregator::getWeight(const std::string &strategy_id) const
{
    // Note: caller must already hold m_mutex.
    auto it = m_weights.find(strategy_id);
    if (m_weights.end() != it)
    {
        return it->second;
    }
    return 1.0; // Default weight.
}

std::int64_t SignalAggregator::nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace pulse::strategy
