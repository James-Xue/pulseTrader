// supertrend_scalper.cpp — SuperTrend indicator strategy (Layer 6 Strategy Engine)

#include "strategy/scalping/supertrend_scalper.hpp"

#include "logging/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pulse::strategy
{

SuperTrendScalper::SuperTrendScalper(const StrategyContext &context)
{
    m_context = context;
}

std::string SuperTrendScalper::name() const
{
    return "SuperTrendScalper";
}

std::string SuperTrendScalper::id() const
{
    return "supertrend_scalper_" + m_context.config.symbol;
}

StrategyParams &SuperTrendScalper::params()
{
    return m_params;
}

void SuperTrendScalper::onTick(const market::Ticker & /*ticker*/)
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

void SuperTrendScalper::onOrderbook(const market::OrderBook & /*book*/)
{
    // This strategy is kline-driven; orderbook updates are ignored.
}

void SuperTrendScalper::onKline(const market::Kline & /*kline*/)
{
    // 1. Read hot-reloadable parameters (lock-free atomic loads).
    const auto period = static_cast<std::size_t>(
        m_params.supertrend_period.load(std::memory_order_acquire));
    const double multiplier = m_params.supertrend_multiplier.load(std::memory_order_acquire);
    const auto cooldown_sec = m_params.cooldown_seconds.load(std::memory_order_acquire);

    // 2. Fetch enough candles to compute ATR and SuperTrend.
    //    Need period + 1 candles: period for ATR, +1 for the "previous close" of the first TR.
    const auto needed = period + 1;
    auto *feed = m_context.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->getKlineBuffer(m_context.config.symbol).snapshot(needed);
    if (candles.size() < needed)
    {
        // Not enough data yet — log warmup progress every 30 seconds.
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (nowMs - m_lastWarmupLogMs >= 30'000)
        {
            PULSE_LOG_INFO("strategy",
                "[{}] Warming up: {}/{} candles accumulated (need ~{} min of kline data)",
                id(), candles.size(), needed, needed);
            m_lastWarmupLogMs = nowMs;
        }
        return;
    }

    // 3. Compute ATR.
    const double atr = computeAtr(candles, period);
    if (0.0 >= atr)
    {
        return; // ATR is zero — market is completely flat, skip.
    }

    // 4. Compute basic bands for the latest candle.
    const auto &latest = candles.back();
    const double midpoint = (latest.high + latest.low) / 2.0;
    const double basic_upper = midpoint + multiplier * atr;
    const double basic_lower = midpoint - multiplier * atr;

    // 5. Compute final bands (tightening logic).
    //    Upper band: take the lower of basic_upper and prev_upper (tighten upward moves).
    //    Reset if previous close broke above the previous upper band.
    double final_upper = basic_upper;
    if (m_hasPrev)
    {
        if (basic_upper < m_prevUpperBand || m_prevClose > m_prevUpperBand)
        {
            final_upper = basic_upper;
        }
        else
        {
            final_upper = m_prevUpperBand;
        }
    }

    //    Lower band: take the higher of basic_lower and prev_lower (tighten downward moves).
    //    Reset if previous close broke below the previous lower band.
    double final_lower = basic_lower;
    if (m_hasPrev)
    {
        if (basic_lower > m_prevLowerBand || m_prevClose < m_prevLowerBand)
        {
            final_lower = basic_lower;
        }
        else
        {
            final_lower = m_prevLowerBand;
        }
    }

    // 6. Determine current SuperTrend value and trend direction.
    bool current_bullish = m_isBullish;
    double current_supertrend = 0.0;

    if (m_hasPrev)
    {
        // If we were bullish and close stays above final_lower → stay bullish.
        if (m_isBullish && latest.close >= final_lower)
        {
            current_bullish = true;
            current_supertrend = final_lower;
        }
        // If we were bearish and close stays below final_upper → stay bearish.
        else if (!m_isBullish && latest.close <= final_upper)
        {
            current_bullish = false;
            current_supertrend = final_upper;
        }
        // Otherwise: trend flipped.
        else if (m_isBullish && latest.close < final_lower)
        {
            current_bullish = false;
            current_supertrend = final_upper;
        }
        else // !m_isBullish && close > final_upper
        {
            current_bullish = true;
            current_supertrend = final_lower;
        }
    }
    else
    {
        // First computation: infer trend from price position relative to midpoint.
        current_bullish = (latest.close >= midpoint);
        current_supertrend = current_bullish ? final_lower : final_upper;
    }

    // 7. Detect trend flip and emit signal.
    if (m_hasPrev)
    {
        const bool flipped_bullish = !m_isBullish && current_bullish;
        const bool flipped_bearish = m_isBullish && !current_bullish;

        if (flipped_bullish || flipped_bearish)
        {
            // Enforce cooldown.
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            const auto cooldown_ms = static_cast<std::int64_t>(cooldown_sec * 1000.0);
            if (nowMs - m_lastSignalTimeMs >= cooldown_ms)
            {
                // 8. Compute confidence: distance from price to SuperTrend, normalized by ATR.
                double confidence = std::abs(latest.close - current_supertrend) / atr;
                confidence = std::clamp(confidence, 0.0, 1.0);

                // 9. Build and emit signal.
                TradingSignal signal;
                signal.type = flipped_bullish ? SignalType::Buy : SignalType::Sell;
                signal.symbol = m_context.config.symbol;
                signal.confidence = confidence;
                signal.price = latest.close;
                signal.strategy_id = id();
                signal.timestamp = now();
                signal.reason = flipped_bullish
                    ? "SuperTrend flipped bullish (price crossed above band)"
                    : "SuperTrend flipped bearish (price crossed below band)";

                PULSE_LOG_INFO("strategy", "[{}] {} signal: confidence={:.4f}, price={:.2f}, atr={:.2f}",
                    id(), signal.reason, confidence, signal.price, atr);

                emitSignal(signal);
                m_lastSignalTimeMs = nowMs;
            }
        }
    }

    // 10. Store state for next candle.
    m_prevUpperBand = final_upper;
    m_prevLowerBand = final_lower;
    m_prevClose = latest.close;
    m_prevSupertrend = current_supertrend;
    m_isBullish = current_bullish;
    m_hasPrev = true;
}

double SuperTrendScalper::computeAtr(const std::vector<market::Kline> &candles,
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
