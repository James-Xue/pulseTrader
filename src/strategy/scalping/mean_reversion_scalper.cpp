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
    m_context = context;
}

std::string MeanReversionScalper::name() const
{
    return "MeanReversionScalper";
}

std::string MeanReversionScalper::id() const
{
    return "mean_reversion_scalper_" + m_context.config.symbol;
}

StrategyParams &MeanReversionScalper::params()
{
    return m_params;
}

void MeanReversionScalper::onTick(const market::Ticker & /*ticker*/)
{
    // This strategy is kline-driven; tick updates are ignored for trading.
    // However, we use onTick() to detect "no kline data at all" (e.g. WS not connected).
    auto *feed = m_context.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->getKlineBuffer(m_context.config.symbol).snapshot(1);
    if (candles.empty())
    {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (nowMs - m_lastNoDataLogMs >= 30'000)
        {
            PULSE_LOG_INFO("strategy",
                "[{}] Waiting for kline data (WS may not be connected yet)", id());
            m_lastNoDataLogMs = nowMs;
        }
    }
}

void MeanReversionScalper::onOrderbook(const market::OrderBook & /*book*/)
{
    // This strategy is kline-driven; orderbook updates are ignored.
}

void MeanReversionScalper::onKline(const market::Kline & /*kline*/)
{
    // 1. Read hot-reloadable parameters.
    const auto bb_period = static_cast<std::size_t>(
        m_params.bb_period.load(std::memory_order_acquire));
    const double bb_std_dev = m_params.bb_std_dev.load(std::memory_order_acquire);
    const double cooldown_sec = m_params.cooldown_seconds.load(std::memory_order_acquire);

    // 2. Fetch enough candles to compute Bollinger Bands.
    auto *feed = m_context.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->getKlineBuffer(m_context.config.symbol).snapshot(bb_period);
    if (candles.size() < bb_period)
    {
        // Not enough data yet — log warmup progress every 30 seconds.
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (nowMs - m_lastWarmupLogMs >= 30'000)
        {
            PULSE_LOG_INFO("strategy",
                "[{}] Warming up: {}/{} candles accumulated (need ~{} min of kline data)",
                id(), candles.size(), bb_period, bb_period);
            m_lastWarmupLogMs = nowMs;
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
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto cooldown_ms = static_cast<std::int64_t>(cooldown_sec * 1000.0);
    if (nowMs - m_lastSignalTimeMs < cooldown_ms)
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
    signal.symbol = m_context.config.symbol;
    signal.confidence = confidence;
    signal.price = current_price;
    signal.strategy_id = id();
    signal.timestamp = now();
    signal.reason = oversold
        ? "Price at/below lower Bollinger Band (oversold, mean reversion expected)"
        : "Price at/above upper Bollinger Band (overbought, mean reversion expected)";

    PULSE_LOG_INFO("strategy", "[{}] {} signal: price={:.2f}, upper={:.2f}, lower={:.2f}, sma={:.2f}",
        id(), signal.reason, current_price, upper_band, lower_band, sma);

    emitSignal(signal);
    m_lastSignalTimeMs = nowMs;
}

} // namespace pulse::strategy
