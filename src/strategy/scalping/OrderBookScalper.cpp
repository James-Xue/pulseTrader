// orderbook_scalper.cpp — Order book imbalance strategy (Layer 6 Strategy Engine)

#include "strategy/scalping/OrderBookScalper.hpp"

#include "logging/Logger.hpp"

#include <chrono>
#include <cmath>

namespace pulse::strategy
{

OrderBookScalper::OrderBookScalper(const StrategyContext &context)
{
    m_context = context;
}

std::string OrderBookScalper::name() const
{
    return "OrderBookScalper";
}

std::string OrderBookScalper::id() const
{
    return "orderbook_scalper_" + m_context.config.symbol;
}

StrategyParams &OrderBookScalper::params()
{
    return m_params;
}

void OrderBookScalper::onTick(const market::Ticker & /*ticker*/)
{
    // This strategy is orderbook-driven; tick updates are ignored.
}

void OrderBookScalper::onKline(const market::Kline & /*kline*/)
{
    // This strategy is orderbook-driven; kline updates are ignored.
}

void OrderBookScalper::onOrderbook(const market::OrderBook &book)
{
    // 1. Read hot-reloadable parameters.
    const auto depth = static_cast<std::size_t>(
        m_params.ob_depth.load(std::memory_order_acquire));
    const double threshold = m_params.ob_imbalance_threshold.load(std::memory_order_acquire);
    const double cooldown_sec = m_params.cooldown_seconds.load(std::memory_order_acquire);

    // 2. Skip if not enough book depth.
    if (book.bids.size() < depth || book.asks.size() < depth)
    {
        return;
    }

    // 3. Sum bid and ask volumes for top N levels.
    double bid_volume = 0.0;
    {
        std::size_t count = 0;
        for (const auto &[price, qty] : book.bids)
        {
            if (count >= depth)
            {
                break;
            }
            bid_volume += qty;
            ++count;
        }
    }

    double ask_volume = 0.0;
    {
        std::size_t count = 0;
        for (const auto &[price, qty] : book.asks)
        {
            if (count >= depth)
            {
                break;
            }
            ask_volume += qty;
            ++count;
        }
    }

    // 4. Skip if no volume on either side.
    const double total_volume = bid_volume + ask_volume;
    if (0.0 == total_volume)
    {
        return;
    }

    // 5. Compute imbalance: positive = buy pressure, negative = sell pressure.
    const double imbalance = (bid_volume - ask_volume) / total_volume;

    // 6. Check if imbalance exceeds threshold.
    if (std::abs(imbalance) < threshold)
    {
        return;
    }

    // 7. Enforce cooldown between signals.
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto cooldown_ms = static_cast<std::int64_t>(cooldown_sec * 1000.0);
    if (nowMs - m_lastSignalTimeMs < cooldown_ms)
    {
        return;
    }

    // 8. Build and emit signal.
    const bool buy_signal = imbalance > 0.0;
    TradingSignal signal;
    signal.type = buy_signal ? SignalType::Buy : SignalType::Sell;
    signal.symbol = m_context.config.symbol;
    signal.confidence = std::clamp(std::abs(imbalance), 0.0, 1.0);

    // Use best bid for buy signals, best ask for sell signals.
    signal.price = buy_signal ? book.bids.begin()->first : book.asks.begin()->first;
    signal.strategy_id = id();
    signal.timestamp = now();
    signal.reason = buy_signal
        ? "Order book buy pressure (imbalance > threshold)"
        : "Order book sell pressure (imbalance < -threshold)";

    PULSE_LOG_INFO("strategy", "[{}] {} signal: imbalance={:.4f}, bid_vol={:.4f}, ask_vol={:.4f}",
        id(), signal.reason, imbalance, bid_volume, ask_volume);

    emitSignal(signal);
    m_lastSignalTimeMs = nowMs;
}

} // namespace pulse::strategy
