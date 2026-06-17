// momentum_scalper.cpp — EMA crossover strategy (Layer 6 Strategy Engine)

#include "pulse/strategy/scalping/momentum_scalper.hpp"

#include "pulse/logging/logger.hpp"

#include <algorithm>
#include <cmath>

namespace pulse::strategy
{

MomentumScalper::MomentumScalper(const StrategyContext &context)
{
    context_ = context;
}

std::string MomentumScalper::name() const
{
    return "MomentumScalper";
}

std::string MomentumScalper::id() const
{
    return "momentum_scalper_" + context_.config.symbol;
}

StrategyParams &MomentumScalper::params()
{
    return params_;
}

void MomentumScalper::on_tick(const market::Ticker & /*ticker*/)
{
    // This strategy is kline-driven; tick updates are ignored.
}

void MomentumScalper::on_orderbook(const market::OrderBook & /*book*/)
{
    // This strategy is kline-driven; orderbook updates are ignored.
}

void MomentumScalper::on_kline(const market::Kline & /*kline*/)
{
    // 1. Read hot-reloadable parameters (lock-free atomic loads).
    const auto fast_period = static_cast<std::size_t>(
        params_.ema_fast_period.load(std::memory_order_acquire));
    const auto slow_period = static_cast<std::size_t>(
        params_.ema_slow_period.load(std::memory_order_acquire));

    // 2. Fetch enough candles to compute both EMAs.
    const auto needed = slow_period + 1; // +1 for initial SMA seed
    auto *feed = context_.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->get_kline_buffer(context_.config.symbol).snapshot(needed);
    if (candles.size() < slow_period)
    {
        // Not enough data yet — skip.
        return;
    }

    // 3. Extract close prices from candles.
    std::vector<double> closes;
    closes.reserve(candles.size());
    for (const auto &c : candles)
    {
        closes.push_back(c.close);
    }

    // 4. Compute fast and slow EMA.
    const double ema_fast = compute_ema(closes, static_cast<double>(fast_period), prev_ema_fast_);
    const double ema_slow = compute_ema(closes, static_cast<double>(slow_period), prev_ema_slow_);

    // 5. Detect crossover (requires previous EMA values).
    if (has_prev_)
    {
        const bool bullish_cross = (prev_ema_fast_ <= prev_ema_slow_) && (ema_fast > ema_slow);
        const bool bearish_cross = (prev_ema_fast_ >= prev_ema_slow_) && (ema_fast < ema_slow);

        if (bullish_cross || bearish_cross)
        {
            // 6. Compute confidence: normalized distance between EMAs.
            double confidence = 0.0;
            if (0.0 != ema_slow)
            {
                confidence = std::abs(ema_fast - ema_slow) / ema_slow;
            }
            confidence = std::clamp(confidence, 0.0, 1.0);

            // 7. Build and emit signal.
            TradingSignal signal;
            signal.type = bullish_cross ? SignalType::Buy : SignalType::Sell;
            signal.symbol = context_.config.symbol;
            signal.confidence = confidence;
            signal.price = closes.back();
            signal.strategy_id = id();
            signal.timestamp = now();
            signal.reason = bullish_cross
                ? "EMA bullish crossover (fast > slow)"
                : "EMA bearish crossover (fast < slow)";

            PULSE_LOG_INFO("strategy", "[{}] {} signal: confidence={:.4f}, price={:.2f}",
                id(), signal.reason, confidence, signal.price);

            emit_signal(signal);
        }
    }

    // 8. Store current EMAs for next crossover detection.
    prev_ema_fast_ = ema_fast;
    prev_ema_slow_ = ema_slow;
    has_prev_ = true;
}

double MomentumScalper::compute_ema(const std::vector<double> &closes,
    double period,
    double prev_ema) const
{
    if (closes.empty())
    {
        return 0.0;
    }

    const double k = 2.0 / (period + 1.0);

    // On first call (prev_ema == 0.0), seed with SMA of the first `period` closes.
    double ema = prev_ema;
    std::size_t start = 0;

    if (0.0 == prev_ema && closes.size() >= static_cast<std::size_t>(period))
    {
        // Compute SMA seed.
        double sum = 0.0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(period); ++i)
        {
            sum += closes[i];
        }
        ema = sum / period;
        start = static_cast<std::size_t>(period);
    }

    // Apply EMA formula for remaining closes.
    for (std::size_t i = start; i < closes.size(); ++i)
    {
        ema = closes[i] * k + ema * (1.0 - k);
    }

    return ema;
}

} // namespace pulse::strategy
