// momentum_scalper.cpp — EMA crossover strategy (Layer 6 Strategy Engine)

#include "strategy/scalping/MomentumScalper.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pulse::strategy
{

MomentumScalper::MomentumScalper(const StrategyContext &context)
{
    m_context = context;
}

std::string MomentumScalper::name() const
{
    return "MomentumScalper";
}

std::string MomentumScalper::id() const
{
    return "momentum_scalper_" + m_context.config.symbol;
}

StrategyParams &MomentumScalper::params()
{
    return m_params;
}

void MomentumScalper::onTick(const market::Ticker & /*ticker*/)
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

void MomentumScalper::onOrderbook(const market::OrderBook & /*book*/)
{
    // This strategy is kline-driven; orderbook updates are ignored.
}

void MomentumScalper::onKline(const market::Kline & /*kline*/)
{
    // 1. Read hot-reloadable parameters (lock-free atomic loads).
    const auto fast_period = static_cast<std::size_t>(
        m_params.ema_fast_period.load(std::memory_order_acquire));
    const auto slow_period = static_cast<std::size_t>(
        m_params.ema_slow_period.load(std::memory_order_acquire));

    // 2. Fetch enough candles to compute both EMAs (and ATR14 for confidence
    //    normalization).
    const auto needed = std::max(slow_period + 1, std::size_t{ 15 }); // +1 for EMA SMA seed; 15 for ATR14
    auto *feed = m_context.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->getKlineBuffer(m_context.config.symbol).snapshot(needed);
    if (candles.size() < slow_period)
    {
        // Not enough data yet — log warmup progress every 30 seconds.
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (nowMs - m_lastWarmupLogMs >= 30'000)
        {
            PULSE_LOG_INFO("strategy",
                "[{}] Warming up: {}/{} candles accumulated (need ~{} min of kline data)",
                id(), candles.size(), slow_period, slow_period);
            m_lastWarmupLogMs = nowMs;
        }
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
    const double ema_fast = computeEma(closes, static_cast<double>(fast_period), m_prevEmaFast);
    const double ema_slow = computeEma(closes, static_cast<double>(slow_period), m_prevEmaSlow);

    // 5. Detect crossover (requires previous EMA values).
    if (m_hasPrev)
    {
        const bool bullish_cross = (m_prevEmaFast <= m_prevEmaSlow) && (ema_fast > ema_slow);
        const bool bearish_cross = (m_prevEmaFast >= m_prevEmaSlow) && (ema_fast < ema_slow);

        if (bullish_cross || bearish_cross)
        {
            // 6. Compute confidence: EMA separation normalized by ATR14.
            //    ATR normalization keeps confidence meaningful across price
            //    scales (dividing by price gave ~0.00007 for XAUUSD — always
            //    below min_confidence, so signals were silently dropped).
            const double atr = computeAtr(candles, 14);
            const double confidence = computeConfidence(ema_fast, ema_slow, atr);

            // 7. Build and emit signal.
            TradingSignal signal;
            signal.type = bullish_cross ? SignalType::Buy : SignalType::Sell;
            signal.symbol = m_context.config.symbol;
            signal.confidence = confidence;
            signal.price = closes.back();
            signal.strategy_id = id();
            signal.timestamp = now();
            signal.reason = bullish_cross
                ? "EMA bullish crossover (fast > slow)"
                : "EMA bearish crossover (fast < slow)";
            signal.indicators = {
                { "ema_fast", ema_fast },
                { "ema_slow", ema_slow },
                { "ema_diff", ema_fast - ema_slow },
                { "atr", atr },
            };

            PULSE_LOG_INFO("strategy", "[{}] {} signal: confidence={:.4f}, price={:.2f}",
                id(), signal.reason, confidence, signal.price);

            emitSignal(signal);
        }
    }

    // 8. Store current EMAs for next crossover detection.
    m_prevEmaFast = ema_fast;
    m_prevEmaSlow = ema_slow;
    m_hasPrev = true;
}

double MomentumScalper::computeEma(const std::vector<double> &closes,
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

double MomentumScalper::computeConfidence(const double ema_fast, const double ema_slow, const double atr)
{
    if (atr <= 0.0)
    {
        return 0.0; // Flat market — no meaningful conviction.
    }
    return std::clamp(std::abs(ema_fast - ema_slow) / atr, 0.0, 1.0);
}

double MomentumScalper::computeAtr(const std::vector<market::Kline> &candles,
    std::size_t period) const
{
    // Need at least period + 1 candles to compute `period` true ranges.
    if (candles.size() < period + 1)
    {
        return 0.0;
    }

    // Compute True Range for the last `period` candles.
    // TR = max(high - low, |high - prev_close|, |low - prev_close|)
    double sum_tr = 0.0;
    const auto start = candles.size() - period;
    for (std::size_t i = start; i < candles.size(); ++i)
    {
        const double hl = candles[i].high - candles[i].low;
        const double hpc = std::abs(candles[i].high - candles[i - 1].close);
        const double lpc = std::abs(candles[i].low - candles[i - 1].close);
        sum_tr += std::max({ hl, hpc, lpc });
    }

    return sum_tr / static_cast<double>(period);
}

} // namespace pulse::strategy
