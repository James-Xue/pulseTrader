// mean_reversion_scalper.cpp — Bollinger Band mean-reversion (Layer 6 Strategy Engine)

#include "strategy/scalping/mean_reversion_scalper.hpp"

#include "logging/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace pulse::strategy
{

MeanReversionScalper::MeanReversionScalper(const StrategyContext &context)
{
    context_ = context;
}

std::string MeanReversionScalper::name() const
{
    return "MeanReversionScalper";
}

std::string MeanReversionScalper::id() const
{
    return "mean_reversion_scalper_" + context_.config.symbol;
}

StrategyParams &MeanReversionScalper::params()
{
    return params_;
}

void MeanReversionScalper::on_tick(const market::Ticker & /*ticker*/)
{
    // This strategy is kline-driven; tick updates are ignored for trading.
    // However, we use on_tick() to detect "no kline data at all" (e.g. WS not connected).
    auto *feed = context_.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->get_kline_buffer(context_.config.symbol).snapshot(1);
    if (candles.empty())
    {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (now_ms - last_no_data_log_ms_ >= 30'000)
        {
            PULSE_LOG_INFO("strategy",
                "[{}] Waiting for kline data (WS may not be connected yet)", id());
            last_no_data_log_ms_ = now_ms;
        }
    }
}

void MeanReversionScalper::on_orderbook(const market::OrderBook & /*book*/)
{
    // This strategy is kline-driven; orderbook updates are ignored.
}

void MeanReversionScalper::on_kline(const market::Kline & /*kline*/)
{
    // 1. Read hot-reloadable parameters.
    const auto bb_period = static_cast<std::size_t>(
        params_.bb_period.load(std::memory_order_acquire));
    const double bb_std_dev = params_.bb_std_dev.load(std::memory_order_acquire);
    const double cooldown_sec = params_.cooldown_seconds.load(std::memory_order_acquire);

    // 2. Fetch enough candles to compute Bollinger Bands.
    auto *feed = context_.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->get_kline_buffer(context_.config.symbol).snapshot(bb_period);
    if (candles.size() < bb_period)
    {
        // Not enough data yet — log warmup progress every 30 seconds.
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (now_ms - last_warmup_log_ms_ >= 30'000)
        {
            PULSE_LOG_INFO("strategy",
                "[{}] Warming up: {}/{} candles accumulated (need ~{} min of kline data)",
                id(), candles.size(), bb_period, bb_period);
            last_warmup_log_ms_ = now_ms;
        }
        return;
    }

    // 3. Extract close prices.
    std::vector<double> closes;
    closes.reserve(candles.size());
    for (const auto &c : candles)
    {
        closes.push_back(c.close);
    }

    // 4. Compute SMA (simple moving average).
    const double sum = std::accumulate(closes.begin(), closes.end(), 0.0);
    const double sma = sum / static_cast<double>(closes.size());

    // 5. Compute standard deviation.
    double sq_sum = 0.0;
    for (const double price : closes)
    {
        const double diff = price - sma;
        sq_sum += diff * diff;
    }
    const double stddev = std::sqrt(sq_sum / static_cast<double>(closes.size()));

    // 6. Compute Bollinger Bands.
    const double upper_band = sma + bb_std_dev * stddev;
    const double lower_band = sma - bb_std_dev * stddev;
    const double band_width = upper_band - lower_band;

    // 7. Get latest price (most recent close).
    const double current_price = closes.back();

    // 8. Check for band breach.
    const bool oversold = current_price <= lower_band;
    const bool overbought = current_price >= upper_band;

    if (!oversold && !overbought)
    {
        return; // Price is within bands — no signal.
    }

    // 9. Enforce cooldown between signals.
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto cooldown_ms = static_cast<std::int64_t>(cooldown_sec * 1000.0);
    if (now_ms - last_signal_time_ms_ < cooldown_ms)
    {
        return;
    }

    // 10. Compute confidence: how far price has penetrated beyond the band.
    double confidence = 0.0;
    if (band_width > 0.0)
    {
        if (oversold)
        {
            confidence = (lower_band - current_price) / band_width;
        }
        else
        {
            confidence = (current_price - upper_band) / band_width;
        }
    }
    confidence = std::clamp(confidence, 0.0, 1.0);

    // 11. Build and emit signal.
    TradingSignal signal;
    signal.type = oversold ? SignalType::Buy : SignalType::Sell;
    signal.symbol = context_.config.symbol;
    signal.confidence = confidence;
    signal.price = current_price;
    signal.strategy_id = id();
    signal.timestamp = now();
    signal.reason = oversold
        ? "Price at/below lower Bollinger Band (oversold, mean reversion expected)"
        : "Price at/above upper Bollinger Band (overbought, mean reversion expected)";

    PULSE_LOG_INFO("strategy", "[{}] {} signal: price={:.2f}, upper={:.2f}, lower={:.2f}, sma={:.2f}",
        id(), signal.reason, current_price, upper_band, lower_band, sma);

    emit_signal(signal);
    last_signal_time_ms_ = now_ms;
}

} // namespace pulse::strategy
